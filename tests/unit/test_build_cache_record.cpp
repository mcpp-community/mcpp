#include <gtest/gtest.h>

import std;
import mcpp.build.execute;
import mcpp.build.prepare;
import mcpp.build.graph_shape;

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

TEST(EngineIdentity, TheRunningEngineIsNotDeclined) {
    const auto engine = running_engine();
    EXPECT_FALSE(engine.version.empty());
    EXPECT_FALSE(engine.exe.empty());
    EXPECT_FALSE(engine_declined_because(engine, engine).has_value());
}

TEST(EngineIdentity, AnEntryWithoutTheFieldDeclinesAndSaysWhy) {
    const auto why = engine_declined_because(std::nullopt, running_engine());
    ASSERT_TRUE(why.has_value());
    EXPECT_NE(why->find("predates the engine identity"), std::string::npos) << *why;
}

TEST(EngineIdentity, AnotherVersionDeclinesAndNamesBoth) {
    const auto running = running_engine();
    auto recorded = running;
    recorded.version = "2026.1.1.1";
    const auto why = engine_declined_because(recorded, running);
    ASSERT_TRUE(why.has_value());
    EXPECT_NE(why->find("2026.1.1.1"), std::string::npos) << *why;
    EXPECT_NE(why->find(running.version), std::string::npos) << *why;
}

// The path alone is enough. A binary moved to another directory, or reinstalled
// there, is the same version and leaves the same stale text in the graph that a
// new version does: the graph names the executable by absolute path.
TEST(EngineIdentity, AnotherPathAtTheSameVersionDeclinesAndNamesBoth) {
    const auto running = running_engine();
    auto recorded = running;
    recorded.exe = "/removed/install/bin/mcpp";
    const auto why = engine_declined_because(recorded, running);
    ASSERT_TRUE(why.has_value());
    EXPECT_NE(why->find("/removed/install/bin/mcpp"), std::string::npos) << *why;
    EXPECT_NE(why->find(running.exe), std::string::npos) << *why;
}

// ─── the admission predicate ────────────────────────────────────────────────

// The engine is the first thing asked: when it is not the same engine, nothing
// else about the record is read, so the reason is the engine's whatever else is
// wrong with the entry. This is what makes the one predicate cover an upgrade
// for the three fast paths that call it.
TEST(AdmitRecordedBuild, AnotherEngineIsDeclinedBeforeAnythingElseIsRead) {
    auto e = minimal_entry();
    e.engine = running_engine();
    e.engine->version = "2026.1.1.1";
    const auto admitted = admit_recorded_build(e, ReplayAsk{.manifest = "mcpp.toml"});
    ASSERT_FALSE(admitted.has_value());
    EXPECT_NE(admitted.error().find("2026.1.1.1"), std::string::npos) << admitted.error();
}

TEST(AdmitRecordedBuild, AnEntryWithoutTheEngineIsDeclined) {
    auto e = minimal_entry();
    const auto admitted = admit_recorded_build(e, ReplayAsk{.manifest = "mcpp.toml"});
    ASSERT_FALSE(admitted.has_value());
    EXPECT_NE(admitted.error().find("predates the engine identity"), std::string::npos)
        << admitted.error();
}

// With the engine matching, the question moves on to the entry's other fields:
// the decline is no longer the engine's.
TEST(AdmitRecordedBuild, TheSameEngineIsAskedTheNextQuestion) {
    auto e = minimal_entry();
    e.engine = running_engine();
    const auto admitted = admit_recorded_build(e, ReplayAsk{.manifest = "mcpp.toml"});
    ASSERT_FALSE(admitted.has_value());
    EXPECT_EQ(admitted.error().find("engine"), std::string::npos) << admitted.error();
    EXPECT_NE(admitted.error().find("runtime binding"), std::string::npos) << admitted.error();
}

// ─── the record, written and read ───────────────────────────────────────────

TEST(BuildCacheRecord, TheEngineAndEachRootsTablesSurviveAWriteAndARead) {
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
    ASSERT_EQ(read.size(), 1u);
    ASSERT_TRUE(read[0].engine.has_value());
    EXPECT_EQ(*read[0].engine, *e.engine);
    EXPECT_TRUE(read[0].depSourceRootsRecorded);
    EXPECT_EQ(read[0].depSourceRoots, e.depSourceRoots);
    // The fields written after the two above were read too: the blocks do not
    // swallow one another.
    EXPECT_TRUE(read[0].toolchainRecorded);
    EXPECT_EQ(read[0].toolchainRequest, e.toolchainRequest);
}

// A path the line format cannot hold is not written at all, so the entry reads
// as one that predates the list and declines, rather than as a shorter path.
TEST(BuildCacheRecord, ARootWithATabInItsPathIsLeftUnrecorded) {
    Tmp tmp;
    auto e = minimal_entry();
    e.depSourceRoots = {{"/work/a\tb", {".ixx"}, {}}};
    e.depSourceRootsRecorded = true;
    e.toolchainRecorded = true;
    e.toolchainRequest = "cli=;default=gcc@16.1.0";
    write_build_cache_entries(tmp.path / "target" / ".build_cache", {e});
    const auto read = read_build_cache(tmp.path);
    ASSERT_EQ(read.size(), 1u);
    EXPECT_FALSE(read[0].depSourceRootsRecorded);
    EXPECT_TRUE(read[0].depSourceRoots.empty());
    EXPECT_TRUE(read[0].toolchainRecorded);
}

TEST(BuildCacheRecord, AnEmptyListOfRootsIsRecordedNotAbsent) {
    Tmp tmp;
    auto e = minimal_entry();
    e.depSourceRootsRecorded = true;
    write_build_cache_entries(tmp.path / "target" / ".build_cache", {e});
    const auto read = read_build_cache(tmp.path);
    ASSERT_EQ(read.size(), 1u);
    EXPECT_TRUE(read[0].depSourceRootsRecorded);
    EXPECT_TRUE(read[0].depSourceRoots.empty());
    EXPECT_FALSE(read[0].engine.has_value());
}

// An engine that recorded the paths alone wrote `depSourceRoots=`. Its roots
// have no tables, so the block is read past and left unrecorded, and the fields
// after it still parse.
TEST(BuildCacheRecord, TheOlderBlockOfPathsIsReadPastAndUnrecorded) {
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
    ASSERT_EQ(read.size(), 1u);
    EXPECT_FALSE(read[0].depSourceRootsRecorded);
    EXPECT_TRUE(read[0].depSourceRoots.empty());
    EXPECT_FALSE(read[0].engine.has_value());
    EXPECT_EQ(read[0].profile, "dev");
    EXPECT_TRUE(read[0].toolchainRecorded);
    EXPECT_EQ(read[0].toolchainRequest, "cli=;default=gcc@16.1.0");
    EXPECT_TRUE(read[0].xlingsPayloadsRecorded);
}

// A line of the block that is not a path and two lists leaves the block
// unrecorded rather than read with a table nobody wrote.
TEST(BuildCacheRecord, AMalformedRootLineLeavesTheBlockUnrecorded) {
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
    ASSERT_EQ(read.size(), 1u);
    EXPECT_FALSE(read[0].depSourceRootsRecorded);
}

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
TEST(DepSourcesNewerThan, AProvidersOwnExtensionMakesItsEditCount) {
    EditedProvider p;
    EXPECT_TRUE(dep_sources_newer_than({{p.tmp.path, {".ixx"}, {}}}, p.ninjaTime));
}

// A root whose package declares nothing about `.ixx` does not have an `.ixx` of
// interest, whatever any other package declares: the table is the root's own.
TEST(DepSourcesNewerThan, APackageThatDeclaresNothingHasNoInterestInTheFile) {
    EditedProvider p;
    EXPECT_FALSE(dep_sources_newer_than({{p.tmp.path, {}, {}}}, p.ninjaTime));
}

// One root's table does not leak into the next: of two roots holding the same
// file, the one whose package declares the extension is the one that answers.
TEST(DepSourcesNewerThan, EachRootIsClassifiedByItsOwnTable) {
    EditedProvider declares;
    EditedProvider silent;
    const auto ninjaTime = declares.ninjaTime;
    EXPECT_FALSE(dep_sources_newer_than({{silent.tmp.path, {}, {}}}, ninjaTime));
    EXPECT_TRUE(dep_sources_newer_than(
        {{silent.tmp.path, {}, {}}, {declares.tmp.path, {".ixx"}, {}}}, ninjaTime));
}

// What was always swept still is, with any table.
TEST(DepSourcesNewerThan, TheBuiltInInterfaceExtensionIsSweptWithoutADeclaration) {
    EditedProvider p;
    write_file(p.tmp.path / "m.cppm", "export module m;\n");
    std::filesystem::last_write_time(p.tmp.path / "m.cppm", p.ninjaTime + std::chrono::seconds(20));
    EXPECT_TRUE(dep_sources_newer_than({{p.tmp.path, {}, {}}}, p.ninjaTime));
}

TEST(DepSourcesNewerThan, ANewerManifestCounts) {
    EditedProvider p;
    std::filesystem::last_write_time(p.tmp.path / "mcpp.toml", p.ninjaTime + std::chrono::seconds(20));
    EXPECT_TRUE(dep_sources_newer_than({{p.tmp.path, {}, {}}}, p.ninjaTime));
}

// The graph names the engine it runs as `$mcpp`, escaped for ninja; the reader
// returns the path the emitter escaped, so that it compares with
// `mcpp_exe_path()` in generic form.
TEST(ReadEngineBinding, TheEscapedPathIsReadBack) {
    Tmp tmp;
    write_file(tmp.path / "build.ninja",
               "# mcpp:graph=normal\ncxx       = /opt/llvm/bin/clang++\n"
               "mcpp      = C$:/Program$ Files/mcpp$$1/mcpp.exe\n\nrule cxx\n");
    EXPECT_EQ(read_engine_binding(tmp.path / "build.ninja"), "C:/Program Files/mcpp$1/mcpp.exe");
}

// The binding is a command word, quoted for the host (#832): a POSIX graph
// writes single quotes, a Windows graph double quotes.
TEST(ReadEngineBinding, TheQuotedWordIsReadBack) {
    Tmp tmp;
    write_file(tmp.path / "build.ninja",
               "mcpp      = '/tmp/mc probe/it'\\''s/$$1/mcpp'\n");
    EXPECT_EQ(read_engine_binding(tmp.path / "build.ninja"), "/tmp/mc probe/it's/$1/mcpp");
    write_file(tmp.path / "build.ninja",
               "mcpp      = \"C:/Program Files/mcpp/mcpp.exe\"\n");
    EXPECT_EQ(read_engine_binding(tmp.path / "build.ninja"), "C:/Program Files/mcpp/mcpp.exe");
}

TEST(ReadEngineBinding, AGraphWithoutTheBindingSaysNothing) {
    Tmp tmp;
    write_file(tmp.path / "build.ninja", "cxx = clang++\nmcpp_extra = x\nrule cxx\n");
    EXPECT_EQ(read_engine_binding(tmp.path / "build.ninja"), "");
}

TEST(ReadEngineBinding, AMissingFileSaysNothing) {
    Tmp tmp;
    EXPECT_EQ(read_engine_binding(tmp.path / "absent.ninja"), "");
}
