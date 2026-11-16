
import std;
import mcpplibs.mtest;
import mcpp.build.execute;
import mcpp.build.prepare;
import mcpp.build.graph_shape;

using namespace mcpplibs::mtest;

using namespace mcpp::build;

// The record of a build (`target/.build_cache`) and the one predicate every fast
// path asks of it. Three properties are fixed here:
//
//   - the engine that wrote a record is part of it, and a record whose engine is
//     not the running one is not replayed (#757);
//   - each path-dependency root is recorded with, and swept by, the extension
//     table of its own package (#756);
//   - an entry written before either field declines, once, and does not
//     misparse what follows it.

namespace {

struct Tmp {
    std::filesystem::path path;
    Tmp() {
        path = std::filesystem::temp_directory_path()
             / std::format("mcpp_cache_record_test_{}", std::random_device{}());
        std::filesystem::create_directories(path);
    }
    ~Tmp() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

void write_file(const std::filesystem::path& p, std::string_view body) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream os(p, std::ios::binary);
    os << body;
}

BuildCacheEntry minimal_entry() {
    BuildCacheEntry e;
    e.targetTriple = "";
    e.outputDir = "/work/target/x86_64-linux-gnu/abc";
    e.ninjaProgram = "/usr/bin/ninja";
    e.fingerprint = "abc";
    e.runtimeEnvKey = "-";
    return e;
}

}  // namespace

// ─── the engine ─────────────────────────────────────────────────────────────

UTest the_running_engine_is_not_declined{"EngineIdentity/TheRunningEngineIsNotDeclined", [] {
    const auto engine = running_engine();
    expect(!engine.version.empty());
    expect(!engine.exe.empty());
    expect(!(engine_declined_because(engine, engine).has_value()));
}};

UTest an_entry_without_the_field_declines_and_says_why{"EngineIdentity/AnEntryWithoutTheFieldDeclinesAndSaysWhy", [] {
    const auto why = engine_declined_because(std::nullopt, running_engine());
    expect(fatal(why.has_value()));
    if (auto&& failed = expect(ne(why->find("predates the engine identity"), std::string::npos)); !failed) failed << *why;
}};

UTest another_version_declines_and_names_both{"EngineIdentity/AnotherVersionDeclinesAndNamesBoth", [] {
    const auto running = running_engine();
    auto recorded = running;
    recorded.version = "2026.1.1.1";
    const auto why = engine_declined_because(recorded, running);
    expect(fatal(why.has_value()));
    if (auto&& failed = expect(ne(why->find("2026.1.1.1"), std::string::npos)); !failed) failed << *why;
    if (auto&& failed = expect(ne(why->find(running.version), std::string::npos)); !failed) failed << *why;
}};

// The path alone is enough. A binary moved to another directory, or reinstalled
// there, is the same version and leaves the same stale text in the graph that a
// new version does: the graph names the executable by absolute path.
UTest another_path_at_the_same_version_declines_and_names_both{"EngineIdentity/AnotherPathAtTheSameVersionDeclinesAndNamesBoth", [] {
    const auto running = running_engine();
    auto recorded = running;
    recorded.exe = "/removed/install/bin/mcpp";
    const auto why = engine_declined_because(recorded, running);
    expect(fatal(why.has_value()));
    if (auto&& failed = expect(ne(why->find("/removed/install/bin/mcpp"), std::string::npos)); !failed) failed << *why;
    if (auto&& failed = expect(ne(why->find(running.exe), std::string::npos)); !failed) failed << *why;
}};

// ─── the admission predicate ────────────────────────────────────────────────

// The engine is the first thing asked: when it is not the same engine, nothing
// else about the record is read, so the reason is the engine's whatever else is
// wrong with the entry. This is what makes the one predicate cover an upgrade
// for the three fast paths that call it.
UTest another_engine_is_declined_before_anything_else_is_read{"AdmitRecordedBuild/AnotherEngineIsDeclinedBeforeAnythingElseIsRead", [] {
    auto e = minimal_entry();
    e.engine = running_engine();
    e.engine->version = "2026.1.1.1";
    const auto admitted = admit_recorded_build(e, ReplayAsk{.manifest = "mcpp.toml"});
    expect(fatal(!admitted.has_value()));
    if (auto&& failed = expect(ne(admitted.error().find("2026.1.1.1"), std::string::npos)); !failed) failed << admitted.error();
}};

UTest an_entry_without_the_engine_is_declined{"AdmitRecordedBuild/AnEntryWithoutTheEngineIsDeclined", [] {
    auto e = minimal_entry();
    const auto admitted = admit_recorded_build(e, ReplayAsk{.manifest = "mcpp.toml"});
    expect(fatal(!admitted.has_value()));
    if (auto&& failed = expect(ne(admitted.error().find("predates the engine identity"), std::string::npos)); !failed) failed << admitted.error();
}};

// With the engine matching, the question moves on to the entry's other fields:
// the decline is no longer the engine's.
UTest the_same_engine_is_asked_the_next_question{"AdmitRecordedBuild/TheSameEngineIsAskedTheNextQuestion", [] {
    auto e = minimal_entry();
    e.engine = running_engine();
    const auto admitted = admit_recorded_build(e, ReplayAsk{.manifest = "mcpp.toml"});
    expect(fatal(!admitted.has_value()));
    if (auto&& failed = expect(eq(admitted.error().find("engine"), std::string::npos)); !failed) failed << admitted.error();
    if (auto&& failed = expect(ne(admitted.error().find("runtime binding"), std::string::npos)); !failed) failed << admitted.error();
}};

// ─── the record, written and read ───────────────────────────────────────────

UTest the_engine_and_each_roots_tables_survive_a_write_and_a_read{"BuildCacheRecord/TheEngineAndEachRootsTablesSurviveAWriteAndARead", [] {
    Tmp tmp;
    auto e = minimal_entry();
    e.engine = EngineIdentity{"2026.10.2.1", "/opt/with space/bin/mcpp"};
    e.depSourceRoots = {
        {"/work/rules",   {".ixx", ".ccm"}, {}},
        {"/work/plain",   {},               {}},
        {"/work/device",  {},               {".cu"}},
    };
    e.depSourceRootsRecorded = true;
    e.toolchainRecorded = true;
    e.toolchainRequest = "cli=;default=llvm@22.1.8";
    write_build_cache_entries(tmp.path / "target" / ".build_cache", {e});

    const auto read = read_build_cache(tmp.path);
    expect(fatal(eq(read.size(), 1u)));
    expect(fatal(read[0].engine.has_value()));
    expect(eq(*read[0].engine, *e.engine));
    expect(bool(read[0].depSourceRootsRecorded));
    expect(eq(read[0].depSourceRoots, e.depSourceRoots));
    // The fields written after the two above were read too: the blocks do not
    // swallow one another.
    expect(bool(read[0].toolchainRecorded));
    expect(eq(read[0].toolchainRequest, e.toolchainRequest));
}};

// A path the line format cannot hold is not written at all, so the entry reads
// as one that predates the list and declines, rather than as a shorter path.
UTest a_root_with_a_tab_in_its_path_is_left_unrecorded{"BuildCacheRecord/ARootWithATabInItsPathIsLeftUnrecorded", [] {
    Tmp tmp;
    auto e = minimal_entry();
    e.depSourceRoots = {{"/work/a\tb", {".ixx"}, {}}};
    e.depSourceRootsRecorded = true;
    e.toolchainRecorded = true;
    e.toolchainRequest = "cli=;default=gcc@16.1.0";
    write_build_cache_entries(tmp.path / "target" / ".build_cache", {e});
    const auto read = read_build_cache(tmp.path);
    expect(fatal(eq(read.size(), 1u)));
    expect(!(read[0].depSourceRootsRecorded));
    expect(read[0].depSourceRoots.empty());
    expect(bool(read[0].toolchainRecorded));
}};

UTest an_empty_list_of_roots_is_recorded_not_absent{"BuildCacheRecord/AnEmptyListOfRootsIsRecordedNotAbsent", [] {
    Tmp tmp;
    auto e = minimal_entry();
    e.depSourceRootsRecorded = true;
    write_build_cache_entries(tmp.path / "target" / ".build_cache", {e});
    const auto read = read_build_cache(tmp.path);
    expect(fatal(eq(read.size(), 1u)));
    expect(bool(read[0].depSourceRootsRecorded));
    expect(read[0].depSourceRoots.empty());
    expect(!(read[0].engine.has_value()));
}};

// An engine that recorded the paths alone wrote `depSourceRoots=`. Its roots
// have no tables, so the block is read past and left unrecorded, and the fields
// after it still parse.
UTest the_older_block_of_paths_is_read_past_and_unrecorded{"BuildCacheRecord/TheOlderBlockOfPathsIsReadPastAndUnrecorded", [] {
    Tmp tmp;
    write_file(tmp.path / "target" / ".build_cache",
        "[target=]\n"
        "/work/target/x86_64-linux-gnu/abc\n"
        "/usr/bin/ninja\n"
        "abc\n"
        "-\n"
        "\n"
        "runTargets=0\n"
        "runEnv=\n"
        "\n"
        "subos=\n"
        "profile=dev\n"
        "cacheMode=global\n"
        "depSourceRoots=2\n"
        "/work/rules\n"
        "/work/other\n"
        "runner=0\n"
        "runtier=0\n"
        "features=\n"
        "toolchain=cli=;default=gcc@16.1.0\n"
        "xlingsPayloads=0\n");
    const auto read = read_build_cache(tmp.path);
    expect(fatal(eq(read.size(), 1u)));
    expect(!(read[0].depSourceRootsRecorded));
    expect(read[0].depSourceRoots.empty());
    expect(!(read[0].engine.has_value()));
    expect(eq(read[0].profile, "dev"));
    expect(bool(read[0].toolchainRecorded));
    expect(eq(read[0].toolchainRequest, "cli=;default=gcc@16.1.0"));
    expect(bool(read[0].xlingsPayloadsRecorded));
}};

// A line of the block that is not a path and two lists leaves the block
// unrecorded rather than read with a table nobody wrote.
UTest a_malformed_root_line_leaves_the_block_unrecorded{"BuildCacheRecord/AMalformedRootLineLeavesTheBlockUnrecorded", [] {
    Tmp tmp;
    write_file(tmp.path / "target" / ".build_cache",
        "[target=]\n"
        "/work/target/x86_64-linux-gnu/abc\n"
        "/usr/bin/ninja\n"
        "abc\n"
        "-\n"
        "\n"
        "depSources=1\n"
        "/work/rules\n"
        "runner=0\n");
    const auto read = read_build_cache(tmp.path);
    expect(fatal(eq(read.size(), 1u)));
    expect(!(read[0].depSourceRootsRecorded));
}};

// ─── the sweep ──────────────────────────────────────────────────────────────

namespace {

// A dependency tree whose manifest is older than `ninjaTime` and whose one
// source, `rules.ixx`, is newer.
struct EditedProvider {
    Tmp tmp;
    std::filesystem::file_time_type ninjaTime;
    EditedProvider() {
        ninjaTime = std::filesystem::file_time_type::clock::now();
        write_file(tmp.path / "mcpp.toml", "[package]\nname = \"rules\"\n");
        write_file(tmp.path / "rules.ixx", "export module rules;\n");
        std::filesystem::last_write_time(tmp.path / "mcpp.toml", ninjaTime - std::chrono::seconds(20));
        std::filesystem::last_write_time(tmp.path / "rules.ixx", ninjaTime + std::chrono::seconds(20));
    }
};

}  // namespace

// The defect of #756: the provider declares `.ixx` and the consumer does not.
// The root is swept with its own package's table, so the edit is seen.
UTest a_providers_own_extension_makes_its_edit_count{"DepSourcesNewerThan/AProvidersOwnExtensionMakesItsEditCount", [] {
    EditedProvider p;
    expect(bool(dep_sources_newer_than({{p.tmp.path, {".ixx"}, {}}}, p.ninjaTime)));
}};

// A root whose package declares nothing about `.ixx` does not have an `.ixx` of
// interest, whatever any other package declares: the table is the root's own.
UTest a_package_that_declares_nothing_has_no_interest_in_the_file{"DepSourcesNewerThan/APackageThatDeclaresNothingHasNoInterestInTheFile", [] {
    EditedProvider p;
    expect(!dep_sources_newer_than({{p.tmp.path, {}, {}}}, p.ninjaTime));
}};

// One root's table does not leak into the next: of two roots holding the same
// file, the one whose package declares the extension is the one that answers.
UTest each_root_is_classified_by_its_own_table{"DepSourcesNewerThan/EachRootIsClassifiedByItsOwnTable", [] {
    EditedProvider declares;
    EditedProvider silent;
    const auto ninjaTime = declares.ninjaTime;
    expect(!dep_sources_newer_than({{silent.tmp.path, {}, {}}}, ninjaTime));
    expect(bool(dep_sources_newer_than(
        {{silent.tmp.path, {}, {}}, {declares.tmp.path, {".ixx"}, {}}}, ninjaTime)));
}};

// What was always swept still is, with any table.
UTest the_built_in_interface_extension_is_swept_without_a_declaration{"DepSourcesNewerThan/TheBuiltInInterfaceExtensionIsSweptWithoutADeclaration", [] {
    EditedProvider p;
    write_file(p.tmp.path / "m.cppm", "export module m;\n");
    std::filesystem::last_write_time(p.tmp.path / "m.cppm", p.ninjaTime + std::chrono::seconds(20));
    expect(bool(dep_sources_newer_than({{p.tmp.path, {}, {}}}, p.ninjaTime)));
}};

UTest a_newer_manifest_counts{"DepSourcesNewerThan/ANewerManifestCounts", [] {
    EditedProvider p;
    std::filesystem::last_write_time(p.tmp.path / "mcpp.toml", p.ninjaTime + std::chrono::seconds(20));
    expect(bool(dep_sources_newer_than({{p.tmp.path, {}, {}}}, p.ninjaTime)));
}};

// The graph names the engine it runs as `$mcpp`, escaped for ninja; the reader
// returns the path the emitter escaped, so that it compares with
// `mcpp_exe_path()` in generic form.
UTest the_escaped_path_is_read_back{"ReadEngineBinding/TheEscapedPathIsReadBack", [] {
    Tmp tmp;
    write_file(tmp.path / "build.ninja",
               "# mcpp:graph=normal\ncxx       = /opt/llvm/bin/clang++\n"
               "mcpp      = C$:/Program$ Files/mcpp$$1/mcpp.exe\n\nrule cxx\n");
    expect(eq(read_engine_binding(tmp.path / "build.ninja"), "C:/Program Files/mcpp$1/mcpp.exe"));
}};

// The binding is a command word, quoted for the host (#832): a POSIX graph
// writes single quotes, a Windows graph double quotes.
UTest the_quoted_word_is_read_back{"ReadEngineBinding/TheQuotedWordIsReadBack", [] {
    Tmp tmp;
    write_file(tmp.path / "build.ninja",
               "mcpp      = '/tmp/mc probe/it'\\''s/$$1/mcpp'\n");
    expect(eq(read_engine_binding(tmp.path / "build.ninja"), "/tmp/mc probe/it's/$1/mcpp"));
    write_file(tmp.path / "build.ninja",
               "mcpp      = \"C:/Program Files/mcpp/mcpp.exe\"\n");
    expect(eq(read_engine_binding(tmp.path / "build.ninja"), "C:/Program Files/mcpp/mcpp.exe"));
}};

UTest a_graph_without_the_binding_says_nothing{"ReadEngineBinding/AGraphWithoutTheBindingSaysNothing", [] {
    Tmp tmp;
    write_file(tmp.path / "build.ninja", "cxx = clang++\nmcpp_extra = x\nrule cxx\n");
    expect(eq(read_engine_binding(tmp.path / "build.ninja"), ""));
}};

UTest a_missing_file_says_nothing{"ReadEngineBinding/AMissingFileSaysNothing", [] {
    Tmp tmp;
    expect(eq(read_engine_binding(tmp.path / "absent.ninja"), ""));
}};
