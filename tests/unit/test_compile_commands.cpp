
import std;
import mcpplibs.mtest;
import mcpp.source_kind;
import mcpp.build.compile_commands;
import mcpp.build.flags;
import mcpp.build.plan;
import nlohmann.json;
import mcpp.manifest.flag_words;
import mcpplibs.os.platform;
import mcpp.toolchain.model;

using namespace mcpplibs::mtest;

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
    expect(!(nlohmann::json::parse(s, nullptr, /*allow_exceptions=*/false)
                     .is_discarded())) << s;
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
UTest preserves_prior_entries_for_files_not_in_fresh_plan{"CompileCommandsMerge/PreservesPriorEntriesForFilesNotInFreshPlan", [] {
    auto fresh    = cdb({ entry("/p/src/main.cpp", "-O2-FRESH") });
    auto existing = cdb({ entry("/p/src/main.cpp", "-O0-STALE"),
                          entry("/p/tests/test_smoke.cpp", "-Igtest") });

    auto merged = merge_compile_commands(
        fresh, existing, [](const std::filesystem::path&) { return true; });

    // main.cpp takes the fresh plan's flags, not the stale prior ones.
    expect(ne(merged.find("-O2-FRESH"), std::string::npos)) << merged;
    expect(eq(merged.find("-O0-STALE"), std::string::npos)) << merged;
    // The test entry, absent from the fresh plan, is preserved.
    expect(ne(merged.find("tests/test_smoke.cpp"), std::string::npos)) << merged;
    expect(ne(merged.find("-Igtest"), std::string::npos)) << merged;
}};

// Prior entries for files that no longer exist on disk must be pruned, so the
// CDB never accumulates dead references (e.g. a deleted test file).
UTest prunes_prior_entries_whose_file_no_longer_exists{"CompileCommandsMerge/PrunesPriorEntriesWhoseFileNoLongerExists", [] {
    auto fresh    = cdb({ entry("/p/src/main.cpp", "-O2") });
    auto existing = cdb({ entry("/p/tests/deleted.cpp", "-Igtest") });

    auto merged = merge_compile_commands(
        fresh, existing,
        [](const std::filesystem::path& p) { return p != "/p/tests/deleted.cpp"; });

    expect(eq(merged.find("deleted.cpp"), std::string::npos)) << merged;
}};

// Exactly one entry per file: the fresh plan wins, no duplicate accrues.
UTest fresh_entry_wins_and_no_duplicate_per_file{"CompileCommandsMerge/FreshEntryWinsAndNoDuplicatePerFile", [] {
    auto fresh    = cdb({ entry("/p/a.cpp", "-FRESH") });
    auto existing = cdb({ entry("/p/a.cpp", "-STALE") });

    auto merged = merge_compile_commands(
        fresh, existing, [](const std::filesystem::path&) { return true; });

    expect(eq(count(merged, "/p/a.cpp"), 1u)) << merged;
    expect(ne(merged.find("-FRESH"), std::string::npos)) << merged;
    expect(eq(merged.find("-STALE"), std::string::npos)) << merged;
}};

// §3.2 item 1: the within-configuration merge's identity is `file` resolved
// against `directory`, not `file` alone. A prior entry with a RELATIVE `file`
// (the shape a unit whose `directory` is not the process's own can write —
// the standard-library units always have one, §3.3) must still be recognized
// as the SAME file the fresh plan already covers, so it is dropped rather
// than kept as a spurious duplicate.
UTest resolves_file_against_directory_for_the_dedup_key{"CompileCommandsMerge/ResolvesFileAgainstDirectoryForTheDedupKey", [] {
    auto fresh = cdb({ entry("/p/src/main.cpp", "-FRESH") });
    nlohmann::json e;
    e["directory"] = "/p";
    e["file"]      = "src/main.cpp";
    e["arguments"] = nlohmann::json::array({ "g++", "-STALE", "-c", "src/main.cpp" });
    e["output"]    = "o";
    auto existing = cdb({ e.dump() });

    auto merged = merge_compile_commands(
        fresh, existing, [](const std::filesystem::path&) { return true; });

    expect(eq(count(merged, "main.cpp"), 1u)) << merged;
    expect(ne(merged.find("-FRESH"), std::string::npos)) << merged;
    expect(eq(merged.find("-STALE"), std::string::npos)) << merged;
}};

// A malformed prior CDB must never break generation: fall back to fresh.
UTest malformed_existing_falls_back_to_fresh{"CompileCommandsMerge/MalformedExistingFallsBackToFresh", [] {
    auto fresh = cdb({ entry("/p/src/main.cpp", "-O2") });

    auto merged = merge_compile_commands(
        fresh, "{ this is not valid json ][",
        [](const std::filesystem::path&) { return true; });

    expect(ne(merged.find("src/main.cpp"), std::string::npos)) << merged;
    expect(ne(merged.find("-O2"), std::string::npos)) << merged;
}};

UTest sorts_final_entries_by_file{"CompileCommandsMerge/SortsFinalEntriesByFile", [] {
    auto fresh = cdb({entry("/p/z.cpp", "-DZ"), entry("/p/a.cpp", "-DA")});
    auto merged = merge_compile_commands(
        fresh, "[]", [](const std::filesystem::path&) { return true; });

    expect(lt(merged.find("/p/a.cpp"), merged.find("/p/z.cpp")));
}};

UTest rejects_non_array_fresh_json{"CompileCommandsWriter/RejectsNonArrayFreshJson", [] {
    TempDir temp;
    auto result = publish_compile_commands(
        temp.path / "compile_commands.json", R"({"file":"not-an-array"})",
        [](const std::filesystem::path&) { return true; });

    expect(fatal(!result.has_value()));
    expect(ne(result.error().message.find("JSON array"), std::string::npos));
}};

UTest publishes_fresh_database_when_none_exists{"CompileCommandsWriter/PublishesFreshDatabaseWhenNoneExists", [] {
    TempDir temp;
    auto path = temp.path / "compile_commands.json";  // deliberately absent
    auto content = cdb({entry("/p/src/main.cpp", "-O2")});

    auto result = publish_compile_commands(
        path, content, [](const std::filesystem::path&) { return true; });

    if (auto&& failed = expect(fatal(result.has_value())); !failed) failed << result.error().message;
    expect(bool(result->changed));
    // The file is re-serialised via nlohmann (sorted + 2-space indent), so
    // assert on content, not on byte-equality with the raw input.
    auto published = read_file(path);
    expect(ne(published.find("/p/src/main.cpp"), std::string::npos)) << published;
    expect(ne(published.find("-O2"), std::string::npos)) << published;
}};

UTest unchanged_content_keeps_mtime{"CompileCommandsWriter/UnchangedContentKeepsMtime", [] {
    TempDir temp;
    auto path = temp.path / "compile_commands.json";
    auto content = cdb({entry((temp.path / "a.cpp").string(), "-DOK")});
    std::ofstream(path, std::ios::binary) << content;
    auto before = std::filesystem::last_write_time(path);

    auto result = publish_compile_commands(
        path, content, [](const std::filesystem::path&) { return true; });

    if (auto&& failed = expect(fatal(result.has_value())); !failed) failed << result.error().message;
    expect(!result->changed);
    expect(eq(std::filesystem::last_write_time(path), before));
}};

UTest replacement_failure_preserves_old_database{"CompileCommandsWriter/ReplacementFailurePreservesOldDatabase", [] {
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

    expect(fatal(!result.has_value()));
    expect(eq(read_file(path), oldContent));
    expect(eq(std::distance(std::filesystem::directory_iterator(temp.path),
                            std::filesystem::directory_iterator{}), 1));
}};

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
UTest normalized_file_keys_heal_stale_separator_spellings{"CompileCommandsMerge/NormalizedFileKeysHealStaleSeparatorSpellings", [] {
    auto p = std::filesystem::path("/p") / "generated" / "modules" / "a.cpp";
    auto stale = "/p/generated/./modules/a.cpp";
    expect(fatal(ne(p.string(), stale))) << "fixture must differ from the fresh spelling";

    auto fresh    = cdb({ entry(p.string(), "-O2-FRESH") });
    auto existing = cdb({ entry(stale, "-O0-STALE") });

    auto merged = merge_compile_commands(
        fresh, existing, [](const std::filesystem::path&) { return true; });

    expect(eq(count(merged, "generated"), 1u)) << merged;
    expect(ne(merged.find("-O2-FRESH"), std::string::npos)) << merged;
    expect(eq(merged.find("-O0-STALE"), std::string::npos)) << merged;
}};

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
UTest strips_posix_quoting_from_a_token{"CompileCommandsArgs/StripsPosixQuotingFromAToken", [] {
    if constexpr (mcpplibs::os::is_windows) {
        skip("single quotes are literal under the MSVCRT rules");
    }
    auto out = mcpp::build::split_flags("-O2 '-fprebuilt-module-path=/a/b' -g");
    expect(fatal(eq(out.size(), 3u)));
    expect(eq(out[1], "-fprebuilt-module-path=/a/b"));
    for (auto const& t : out) expect(!has_edge_quote(t)) << t;
}};

UTest strips_windows_quoting_from_a_token{"CompileCommandsArgs/StripsWindowsQuotingFromAToken", [] {
    auto out = mcpp::build::split_flags(R"(-O2 "-IC:\Users\x\inc" -g)");
    expect(fatal(eq(out.size(), 3u)));
    expect(eq(out[1], R"(-IC:\Users\x\inc)"));
    for (auto const& t : out) expect(!has_edge_quote(t)) << t;
}};

// The one that bit: a quoted path containing a space is ONE argument. The
// space arrives ninja-escaped (`$ `) because flags.cppm escapes before it
// quotes, so the un-escape has to happen INSIDE the quotes — do it in the
// wrong order and the token splits exactly here.
UTest a_quoted_path_with_a_space_stays_one_token{"CompileCommandsArgs/AQuotedPathWithASpaceStaysOneToken", [] {
    // The quotes are the host's: flags.cppm quotes with `'` on POSIX and `"`
    // under the MSVCRT rules.
    const std::string_view line = mcpplibs::os::is_windows
        ? "-O2 \"-fmodule-file=std=/tmp/my$ project/std.pcm\" -g"
        : "-O2 '-fmodule-file=std=/tmp/my$ project/std.pcm' -g";
    auto out = mcpp::build::split_flags(line);
    expect(fatal(eq(out.size(), 3u))) << "the quoted path was split";
    expect(eq(out[1], "-fmodule-file=std=/tmp/my project/std.pcm"));
    for (auto const& t : out) expect(!has_edge_quote(t)) << t;
}};

// Unquoted input: ninja's escapes are undone first, and then the host splits
// the line. `$ ` is a plain space by the time the host reads it, so an
// unquoted `-Idir$ with$ space` is three words to the compiler, and the
// database says so. The engine quotes every word it writes (include_token,
// ninja_command_word); the quoted spelling is the test above.
UTest unquoted_input_splits_where_the_host_splits{"CompileCommandsArgs/UnquotedInputSplitsWhereTheHostSplits", [] {
    auto out = mcpp::build::split_flags("-IC$:/x -DA=1 -Idir$ with$ space");
    expect(fatal(eq(out.size(), 5u)));
    expect(eq(out[0], "-IC:/x"));
    expect(eq(out[1], "-DA=1"));
    expect(eq(out[2], "-Idir"));
    expect(eq(out[3], "with"));
    expect(eq(out[4], "space"));
}};

// A quote inside a word is quoting on both hosts' readers: `sh` and the MSVCRT
// rules both hand the compiler `-DGREETING=hi` for this text. The version of
// this test before #655 asserted the opposite, which pinned the compile
// database to a reading no host applies.
UTest quotes_inside_a_word_are_quoting{"CompileCommandsArgs/QuotesInsideAWordAreQuoting", [] {
    auto out = mcpp::build::split_flags(R"(-DGREETING="hi")");
    expect(fatal(eq(out.size(), 1u)));
    expect(eq(out[0], "-DGREETING=hi"));
}};

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

UTest the_edge_and_the_database_list_the_same_words{"CompileCommandsArgs/TheEdgeAndTheDatabaseListTheSameWords", [] {
    const auto words = mcpp::manifest::flag_words(kIssue655Elements);
    expect(eq(mcpp::build::split_flags(mcpp::build::ninja_flag_list(kIssue655Elements)), words));
    const std::vector<std::string> want{
        R"(-DPLATFORM_CONFIG_H="mcpp_libarchive_config.h")", "-DMID=mid", "-DSQ=sq",
        "-include", "mcpp_lua_platform_config.h", "-I/opt/my dir/include",
        R"(-IC:\Users\x\include)", "-DDOLLAR=a$b", R"(-DDEF="def")", "-DSPACE=a b"};
    expect(eq(words, want));
}};

UTest a_word_on_an_edge_reads_back_as_the_word{"CompileCommandsArgs/AWordOnAnEdgeReadsBackAsTheWord", [] {
    for (auto const& w : random_words()) {
        if (auto&& failed = expect(eq(mcpp::build::split_flags(mcpp::build::ninja_command_word(w)), std::vector<std::string>{w})); !failed) failed << "word: [" << w << "]";
    }
}};

// The reader above is mcpp's model of the host. On a POSIX host the host itself
// is available, so the model is checked against it: `/bin/sh` receives the
// quoted words and prints each one back.
UTest shell_receives_the_quoted_words_as_written{"CompileCommandsArgs/ShellReceivesTheQuotedWordsAsWritten", [] {
    if constexpr (mcpplibs::os::is_windows) {
        skip("the POSIX shell is the reader measured here");
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
    expect(fatal(eq(std::system(command.c_str()), 0)));
    std::ifstream in(out);
    std::vector<std::string> printed;
    for (std::string line; std::getline(in, line);) printed.push_back(line);
    expect(eq(printed, words));
}};

UTest a_unit_lists_the_words_of_its_flag_list{"CompileCommandsEmit/AUnitListsTheWordsOfItsFlagList", [] {
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
    expect(ne(at, args.end())) << "the unit's words are not listed in order";
}};

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
UTest emitted_paths_use_native_separators{"CompileCommandsEmit/EmittedPathsUseNativeSeparators", [] {
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

    // C3 (design 2026-09-26 §3.3): `directory` is the OUTPUT directory the
    // compiler runs in, not `plan.projectRoot` — here `/p/target`.
    if constexpr (std::filesystem::path::preferred_separator == '\\') {
        expect(eq(e["file"].get<std::string>(), "C:\\Users\\x\\src\\main.cpp"));
        expect(eq(e["directory"].get<std::string>(), "\\p\\target"));
        expect(eq(e["output"].get<std::string>(), "\\p\\target\\obj\\main.o"));
        expect(eq(inc, "-IC:\\proj\\generated\\inc"));
        expect(eq(incAfter, "-idirafterC:\\proj\\third_party\\inc"));
    } else {
        expect(eq(e["file"].get<std::string>(), "C:/Users/x/src/main.cpp"));
        expect(eq(e["directory"].get<std::string>(), "/p/target"));
        expect(eq(inc, "-IC:/proj/generated/inc"));
        expect(eq(incAfter, "-idirafterC:/proj/third_party/inc"));
    }
}};

UTest replaces_symlink_target_without_removing_link{"CompileCommandsWriter/ReplacesSymlinkTargetWithoutRemovingLink", [] {
    TempDir temp;
    auto target = temp.path / "build" / "compile_commands.json";
    auto link = temp.path / "compile_commands.json";
    std::filesystem::create_directories(target.parent_path());
    auto oldContent = cdb({entry((temp.path / "old.cpp").string(), "-DOLD")});
    auto newContent = cdb({entry((temp.path / "new.cpp").string(), "-DNEW")});
    std::ofstream(target) << oldContent;
    std::error_code symlinkEc;
    std::filesystem::create_symlink(target, link, symlinkEc);
    if (symlinkEc) skip([&] { std::ostringstream text; text << "symlink unavailable: " << symlinkEc.message(); return text.str(); }());

    auto result = publish_compile_commands(
        link, newContent, [](const std::filesystem::path&) { return false; });

    if (auto&& failed = expect(fatal(result.has_value())); !failed) failed << result.error().message;
    expect(std::filesystem::is_symlink(link));
    auto published = read_file(target);
    expect(ne(published.find("-DNEW"), std::string::npos));
    expect(eq(published.find("-DOLD"), std::string::npos));
}};

UTest existing_unreadable_database_is_not_overwritten{"CompileCommandsWriter/ExistingUnreadableDatabaseIsNotOverwritten", [] {
    TempDir temp;
    auto path = temp.path / "compile_commands.json";
    std::ofstream(path) << "last-known-good";
    std::error_code permissionEc;
    std::filesystem::permissions(
        path, std::filesystem::perms::owner_read,
        std::filesystem::perm_options::remove, permissionEc);
    if (permissionEc) skip([&] { std::ostringstream text; text << "cannot change permissions: " << permissionEc.message(); return text.str(); }());
    std::ifstream probe(path);
    if (probe) {
        std::filesystem::permissions(path, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace, permissionEc);
        skip("test user can still read restricted file");
    }

    auto result = publish_compile_commands(
        path, cdb({entry((temp.path / "new.cpp").string(), "-DNEW")}),
        [](const std::filesystem::path&) { return true; });

    expect(fatal(!result.has_value()));
    expect(ne(result.error().message.find("read existing"), std::string::npos));
    std::filesystem::permissions(path, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace, permissionEc);
}};

// ── One record, two databases ───────────────────────────────────────────────
//
// The compile database and the build database render the same per-unit record,
// so `arguments` cannot differ between them. The second half shows the
// comparison can fail: the record under other flags is not the CDB's.
UTest unit_invocations_are_the_compile_database_arguments{"CompileCommandsEmit/UnitInvocationsAreTheCompileDatabaseArguments", [] {
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
    expect(fatal(eq(j.size(), invs.size())));
    for (std::size_t i = 0; i < invs.size(); ++i) {
        expect(eq(j[i]["arguments"].get<std::vector<std::string>>(), invs[i].arguments));
        expect(eq(j[i]["file"].get<std::string>(), invs[i].file));
        expect(eq(j[i]["directory"].get<std::string>(), invs[i].directory));
        expect(eq(j[i]["output"].get<std::string>(), invs[i].output));
        expect(eq(invs[i].unit, &plan.compileUnits[i]));
    }

    CompileFlags other = flags;
    other.cxx = "-std=c++26";
    expect(ne(j[0]["arguments"].get<std::vector<std::string>>(), unit_invocations(plan, other)[0].arguments));
}};

// ── C3: `directory` is the output directory ─────────────────────────────────
//
// The JSON format and S1-8-2 define the field as the directory the compiler
// runs in; ninja runs every compile in `plan.outputDir`, never the project
// root (design 2026-09-26 §3.3).
UTest a_project_units_directory_is_the_output_directory{"CompileCommandsEmit/AProjectUnitsDirectoryIsTheOutputDirectory", [] {
    BuildPlan plan;
    plan.projectRoot = "/p";
    plan.outputDir = "/p/target/x86_64-linux-gnu/deadbeef";
    plan.compileUnits.push_back({
        .source = std::filesystem::path("/p/src/main.cpp"),
        .kind = mcpp::SourceKind::Cxx,
        .object = std::filesystem::path("obj") / "main.o",
        .packageName = "demo",
    });
    CompileFlags flags;
    flags.cxxBinary = "/usr/bin/g++";

    auto invs = unit_invocations(plan, flags);
    expect(fatal(eq(invs.size(), 1u)));
    // Compared as paths: the field is written in the native spelling, which
    // on Windows turns this fixture's `/` into `\`.
    expect(eq(std::filesystem::path(invs[0].directory), plan.outputDir));
    expect(ne(std::filesystem::path(invs[0].directory), plan.projectRoot));
}};

// ── C4: the module interface language flag ──────────────────────────────────
//
// BmiTraits::moduleInterfaceLangFlag is stated at the position the build uses
// (immediately before `-c`) for every unit that actually provides a module;
// a unit that does not (an ordinary source, here) carries no such flag at
// all (design 2026-09-26 §3.4).
UTest interface_unit_carries_the_language_flag_before_dash_c_and_implementation_does_not{"CompileCommandsEmit/InterfaceUnitCarriesTheLanguageFlagBeforeDashCAndImplementationDoesNot", [] {
    BuildPlan plan;
    plan.projectRoot = "/p";
    plan.outputDir = "/p/target";
    plan.toolchain.compiler = mcpp::toolchain::CompilerId::Clang;
    plan.compileUnits.push_back({
        .source = std::filesystem::path("/p/src/greet.cppm"),
        .kind = mcpp::SourceKind::ModuleInterface,
        .object = std::filesystem::path("obj") / "greet.o",
        .packageName = "demo",
        .providesModule = "demo.greet",
    });
    plan.compileUnits.push_back({
        .source = std::filesystem::path("/p/src/main.cpp"),
        .kind = mcpp::SourceKind::Cxx,
        .object = std::filesystem::path("obj") / "main.o",
        .packageName = "demo",
    });
    CompileFlags flags;
    flags.cxxBinary = "/usr/bin/clang++";

    auto invs = unit_invocations(plan, flags);
    expect(fatal(eq(invs.size(), 2u)));

    auto& iface = invs[0].arguments;
    auto ifaceC = std::ranges::find(iface, std::string("-c"));
    expect(fatal(ne(ifaceC, iface.end())));
    expect(fatal(ge(std::distance(iface.begin(), ifaceC), 2)));
    expect(eq(*(ifaceC - 2), "-x"));
    expect(eq(*(ifaceC - 1), "c++-module"));

    auto& impl = invs[1].arguments;
    expect(eq(std::ranges::find(impl, std::string("-x")), impl.end()));
}};

// MSVC's form (`/interface /TP`) stays out of the record until a Windows
// measurement of clang-cl-mode clangd answers whether it accepts `/interface`
// (design §3.4, "Open"); until then an MSVC plan carries no language flag at
// all, matching the implementation-unit case above rather than guessing.
UTest msvc_carries_no_language_flag_yet{"CompileCommandsEmit/MsvcCarriesNoLanguageFlagYet", [] {
    BuildPlan plan;
    plan.projectRoot = "/p";
    plan.outputDir = "/p/target";
    plan.toolchain.compiler = mcpp::toolchain::CompilerId::MSVC;
    plan.compileUnits.push_back({
        .source = std::filesystem::path("/p/src/greet.cppm"),
        .kind = mcpp::SourceKind::ModuleInterface,
        .object = std::filesystem::path("obj") / "greet.o",
        .packageName = "demo",
        .providesModule = "demo.greet",
    });
    CompileFlags flags;
    flags.cxxBinary = "/vs/cl.exe";

    auto invs = unit_invocations(plan, flags);
    expect(fatal(eq(invs.size(), 1u)));
    expect(eq(std::ranges::find(invs[0].arguments, std::string("/interface")), invs[0].arguments.end()));
}};
