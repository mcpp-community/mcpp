#include <fcntl.h>
#if !defined(_WIN32)
#include <sys/file.h>
#include <unistd.h>
#endif

import std;
import mcpplibs.mtest;
import mcpp.bmi_cache;
import mcpp.bmi_cache.maintenance;
import mcpp.home;
import mcpp.invocation;
import nlohmann.json;
import mcpplibs.os.env;
import mcpplibs.os.platform;
import mcpplibs.os.windows;

using namespace mcpplibs::mtest;

using namespace mcpp::bmi_cache;

UTest runner_cache_path_does_not_use_the_windows_ansi_code_page{"BMICacheMaintenance/RunnerCachePathDoesNotUseTheWindowsAnsiCodePage", [] {
    if constexpr (!mcpplibs::os::is_windows) {
        skip("path::string() only converts through an ANSI code page on Windows");
    } else {
        const auto acp = mcpplibs::os::windows::code_page(); // GetACP, including a UTF-8 process manifest.
        if (acp == 65001) skip("UTF-8 ACP can represent every path");
        const std::filesystem::path name(std::wstring{wchar_t(0x0915), wchar_t(0x0916), wchar_t(0x0917)});
        bool narrowing_throws = false;
        try { (void)name.string(); }
        catch (const std::exception&) { narrowing_throws = true; }
        expect(fatal(bool(narrowing_throws))) << "the fixture must be unrepresentable in ACP " << acp;

        const auto scratch = std::filesystem::temp_directory_path()
            / std::format("mcpp-runner-cache-{}", std::random_device{}());
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); }
        } cleanup{scratch};
        const auto home = scratch / name;
        const auto bin = home / "build-cache/v1/tool/mcpplibs/openkal-tools@0.4.2/key/bin";
        std::filesystem::create_directories(bin);
        // A real, bounded Windows program, invoked as `cache gc`. It can fail
        // to find these names: gc still reports that outcome without throwing
        // while preparing the native executable path or its diagnostic.
        const auto where = mcpplibs::os::env::find_executable("where.exe");
        expect(fatal(where.has_value()));
        std::filesystem::copy_file(*where, bin / "okrun.exe");
        mcpp::invocation::ScopedEnv selected("MCPP_HOME", mcpp::home::spelling(home));
        expect(nothrow([&] { { expect(eq(cache_gc("", "3650d"), 0)); }; }));
    }
}};

namespace {

struct Tmp {
    std::filesystem::path path;
    Tmp() {
        auto base = std::filesystem::temp_directory_path()
                  / std::format("mcpp_bmi_cache_test_{}", std::random_device{}());
        std::filesystem::create_directories(base);
        path = base;
    }
    ~Tmp() {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
};

nlohmann::json makeInputs(std::string_view flavor = "base") {
    nlohmann::json j;
    j["epoch"] = 1;
    j["toolchain"] = {{"compiler", "gcc"}, {"compiler_version", "16.1.0"}};
    j["profile"] = {{"opt_level", "2"}, {"debug", false}};
    j["package"] = {{"index", "mcpplibs"}, {"name", "mcpplibs.cmdline"},
                    {"version", "0.0.1"}};
    j["config"] = {{"cxxflags", nlohmann::json::array({std::string(flavor)})}};
    return j;
}

CacheKey makeKey(const std::filesystem::path& root,
                 std::string_view key = "deadbeef0123abcd",
                 std::string_view flavor = "base") {
    return CacheKey{
        .cacheRoot   = root,
        .indexName   = "mcpplibs",
        .packageName = "mcpplibs.cmdline",
        .version     = "0.0.1",
        .keyHex      = std::string(key),
        .inputs      = makeInputs(flavor),
    };
}

void writeFile(const std::filesystem::path& p, std::string_view body) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p) << body;
}

std::string readFile(const std::filesystem::path& p) {
    std::ifstream is(p);
    return std::string((std::istreambuf_iterator<char>(is)), {});
}

nlohmann::json readJson(const std::filesystem::path& p) {
    std::ifstream is(p);
    nlohmann::json j;
    is >> j;
    return j;
}

// `cacheRel` is the entry-internal address, `buildRel` is where this "build"
// produced the file (relative to the project target dir). They are separate on
// purpose — see mcpp#344.
DepArtifacts oneOfEach() {
    return DepArtifacts{ .bmiFiles = {"lib.gcm"},
                         .objFiles = {{"lib.m.o", "obj/lib.m.o"}} };
}

void seedProject(const std::filesystem::path& project) {
    writeFile(project / "gcm.cache" / "lib.gcm", "G");
    writeFile(project / "obj"       / "lib.m.o", "O");
}

} // namespace

UTest entry_dir_is_keyed_by_package_identity_and_key_not_by_project{"BmiCache/EntryDirIsKeyedByPackageIdentityAndKeyNotByProject", [] {
    auto k = makeKey("/home/u/.mcpp/build-cache/v1");
    auto expected = std::filesystem::path(
        "/home/u/.mcpp/build-cache/v1/pkg/mcpplibs/mcpplibs.cmdline@0.0.1/deadbeef0123abcd");
    expect(eq(k.dir(), expected));
    expect(eq(k.entryFile().filename().string(), "entry.json"));
    expect(eq(k.bmiDir().filename().string(), "bmi"));
    expect(eq(k.objDir().filename().string(), "obj"));
}};

// The layout version must be a path segment: replacing the layout has to be a
// new tree, never a migration of (or a deletion of) the old one.
UTest entry_dir_carries_the_layout_version{"BmiCache/EntryDirCarriesTheLayoutVersion", [] {
    auto k = makeKey("/home/u/.mcpp/build-cache/v1");
    auto s = k.dir().generic_string();
    expect(ne(s.find("/build-cache/v1/pkg/"), std::string::npos)) << s;
    // And nothing may land under the pre-v1 root.
    expect(eq(s.find("/.mcpp/bmi/"), std::string::npos)) << s;
}};

UTest is_cached_false_when_entry_missing{"BmiCache/IsCachedFalseWhenEntryMissing", [] {
    Tmp t;
    expect(!(is_cached(makeKey(t.path), oneOfEach())));
}};

UTest populate_writes_self_describing_entry{"BmiCache/PopulateWritesSelfDescribingEntry", [] {
    Tmp t;
    auto home    = t.path / "home";
    auto project = t.path / "proj" / "target";
    seedProject(project);

    auto k = makeKey(home);
    auto pop = populate_from(k, project, oneOfEach());
    if (auto&& failed = expect(fatal(bool(pop))); !failed) failed << pop.error();

    expect(fatal(std::filesystem::exists(k.entryFile())));
    expect(std::filesystem::exists(k.bmiDir() / "lib.gcm"));
    expect(std::filesystem::exists(k.objDir() / "lib.m.o"));
    expect(is_cached(k, oneOfEach()));

    // The entry has to carry the key AND the full inputs: a hit is validated
    // field by field, so a cache whose entries only listed files could never be
    // audited when a wrong hit was suspected.
    auto j = readJson(k.entryFile());
    expect(eq(j.value("key", std::string{}), "deadbeef0123abcd"));
    expect(eq(j["inputs"], makeInputs()));
    expect(eq(j["bmi"].size(), 1u));
    expect(eq(j["obj"].size(), 1u));
    expect(j.contains("created"));
    expect(j.contains("accessed"));
}};

// The whole point of recording inputs: equal hashes are never trusted on their
// own. Same key directory, different inputs ⇒ miss.
UTest is_cached_false_when_recorded_inputs_differ{"BmiCache/IsCachedFalseWhenRecordedInputsDiffer", [] {
    Tmp t;
    auto home    = t.path / "home";
    auto project = t.path / "proj" / "target";
    seedProject(project);

    auto written = makeKey(home, "samekey00000000", "base");
    expect(fatal(bool(populate_from(written, project, oneOfEach()))));
    expect(is_cached(written, oneOfEach()));

    auto probed = makeKey(home, "samekey00000000", "different-flag");
    expect(!(is_cached(probed, oneOfEach())));
}};

// An entry written by an older mcpp may carry extra keys, but every field the
// current build cares about must be present. Missing ⇒ mismatch, never a pass.
UTest is_cached_false_when_a_required_input_field_is_absent{"BmiCache/IsCachedFalseWhenARequiredInputFieldIsAbsent", [] {
    Tmp t;
    auto home    = t.path / "home";
    auto project = t.path / "proj" / "target";
    seedProject(project);

    auto k = makeKey(home);
    expect(fatal(bool(populate_from(k, project, oneOfEach()))));

    auto j = readJson(k.entryFile());
    j["inputs"].erase("profile");
    std::ofstream(k.entryFile()) << j.dump(2);

    expect(!(is_cached(k, oneOfEach())));
}};

UTest is_cached_false_when_schema_differs{"BmiCache/IsCachedFalseWhenSchemaDiffers", [] {
    Tmp t;
    auto home    = t.path / "home";
    auto project = t.path / "proj" / "target";
    seedProject(project);

    auto k = makeKey(home);
    expect(fatal(bool(populate_from(k, project, oneOfEach()))));

    auto j = readJson(k.entryFile());
    j["schema"] = kEntrySchema + 1;
    std::ofstream(k.entryFile()) << j.dump(2);

    expect(!(is_cached(k, oneOfEach())));
}};

UTest is_cached_false_when_sentinel_exists_but_file_missing{"BmiCache/IsCachedFalseWhenSentinelExistsButFileMissing", [] {
    Tmp t;
    auto home    = t.path / "home";
    auto project = t.path / "proj" / "target";
    seedProject(project);

    auto k = makeKey(home);
    expect(fatal(bool(populate_from(k, project, oneOfEach()))));
    expect(fatal(is_cached(k, oneOfEach())));

    std::filesystem::remove(k.objDir() / "lib.m.o");
    expect(!(is_cached(k, oneOfEach())));
}};

UTest resolve_cached_reports_the_artifacts_without_copying{"BmiCache/ResolveCachedReportsTheArtifactsWithoutCopying", [] {
    Tmp t;
    auto home    = t.path / "home";
    auto project = t.path / "proj" / "target";
    seedProject(project);

    auto k = makeKey(home);
    expect(fatal(bool(populate_from(k, project, oneOfEach()))));

    auto arts = resolve_cached(k);
    if (auto&& failed = expect(fatal(bool(arts))); !failed) failed << arts.error();
    expect(eq(arts->bmiFiles, std::vector<std::string>{"lib.gcm"}));
    expect(fatal(eq(arts->objFiles.size(), 1u)));
    expect(eq(arts->objFiles[0].cacheRel, "lib.m.o"));
    // Read-back never yields a build path: the entry does not record one, which
    // is the whole point of mcpp#344.
    expect(arts->objFiles[0].buildRel.empty());

    // resolve_cached must not write into a project dir. Staging is a ninja edge
    // now: copying artifacts in from outside the graph is exactly what made the
    // old cache a no-op — ninja rebuilds any output it has no command-line
    // record for, so the copies were recompiled over on every fresh build dir.
    auto project2 = t.path / "proj2" / "target";
    (void)resolve_cached(k);
    expect(!std::filesystem::exists(project2));
}};

UTest cached_paths_point_into_the_entry{"BmiCache/CachedPathsPointIntoTheEntry", [] {
    auto k = makeKey("/home/u/.mcpp/build-cache/v1");
    expect(eq(cached_bmi_path(k, "lib.gcm"), k.bmiDir() / "lib.gcm"));
    expect(eq(cached_obj_path(k, "sub/lib.m.o"), k.objDir() / "sub" / "lib.m.o"));
}};

// Nested object paths (mcpp#233's per-package object prefixes) must round-trip.
UTest populate_handles_nested_object_paths{"BmiCache/PopulateHandlesNestedObjectPaths", [] {
    Tmp t;
    auto home    = t.path / "home";
    auto project = t.path / "proj" / "target";
    writeFile(project / "obj" / "pkg_zlib" / "zlib-1.3" / "compress.o", "NESTED");

    auto k = makeKey(home);
    DepArtifacts arts {
        .objFiles = {{"zlib-1.3/compress.o", "obj/pkg_zlib/zlib-1.3/compress.o"}} };
    expect(fatal(bool(populate_from(k, project, arts))));
    // Stored at the ENTRY address, read from the BUILD path. The entry knows
    // nothing about the `pkg_zlib/` partition, which is consumer-side.
    expect(eq(readFile(k.objDir() / "zlib-1.3" / "compress.o"), "NESTED"));
    expect(is_cached(k, arts));
    auto j = readJson(k.entryFile());
    expect(eq(j["obj"][0].get<std::string>(), "zlib-1.3/compress.o"));
}};

// mcpp#344, the load-bearing case. Two consumers of ONE key may place the same
// object at different build-dir paths (#233's basename disambiguation fires on
// a census over the whole build dir, which varies with the consumer's package
// mix). The entry is written by whoever runs first; whoever runs second must
// get a clean MISS, with the divergence named — never a hit that goes on to
// stage a file the entry never had, which is what ninja reported as
// "missing and no known rule to make it" at graph load.
UTest probe_misses_when_the_entry_does_not_hold_the_requested_artifacts{"BmiCache/ProbeMissesWhenTheEntryDoesNotHoldTheRequestedArtifacts", [] {
    Tmp t;
    auto home    = t.path / "home";
    auto project = t.path / "proj" / "target";
    writeFile(project / "obj" / "compress.o", "FLAT");

    auto k = makeKey(home);
    // The first consumer populated the entry under a DIFFERENT internal
    // address than the one this build is going to ask for.
    DepArtifacts written { .objFiles = {{"other/compress.o", "obj/compress.o"}} };
    expect(fatal(bool(populate_from(k, project, written))));

    DepArtifacts wanted { .objFiles = {{"zlib-1.3/compress.o", "obj/compress.o"}} };
    auto probe = probe_cached(k, wanted);
    expect(!probe.ok);
    expect(fatal(eq(probe.layoutMismatch.size(), 1u)));
    expect(eq(probe.layoutMismatch[0], "zlib-1.3/compress.o"));
}};

// The same asymmetry on the read side: an entry that lists a file it does not
// have on disk must not be reported as a layout divergence OR as a hit.
UTest probe_misses_when_a_requested_file_is_listed_but_absent{"BmiCache/ProbeMissesWhenARequestedFileIsListedButAbsent", [] {
    Tmp t;
    auto home    = t.path / "home";
    auto project = t.path / "proj" / "target";
    seedProject(project);

    auto k = makeKey(home);
    expect(fatal(bool(populate_from(k, project, oneOfEach()))));
    std::filesystem::remove(k.objDir() / "lib.m.o");

    auto probe = probe_cached(k, oneOfEach());
    expect(!probe.ok);
    expect(fatal(eq(probe.layoutMismatch.size(), 1u)));
    expect(eq(probe.layoutMismatch[0], "lib.m.o"));
}};

// An entry whose own validation fails (wrong inputs) is an ordinary miss and
// carries NO layout complaint — the caller warns on layout divergence only, and
// warning on every routine miss would make the signal worthless.
UTest probe_reports_no_layout_mismatch_when_the_entry_itself_is_invalid{"BmiCache/ProbeReportsNoLayoutMismatchWhenTheEntryItselfIsInvalid", [] {
    Tmp t;
    auto home    = t.path / "home";
    auto project = t.path / "proj" / "target";
    seedProject(project);

    expect(fatal(bool(populate_from(makeKey(home, "samekey00000000", "base"),
                              project, oneOfEach()))));
    auto probe = probe_cached(makeKey(home, "samekey00000000", "other"),
                              oneOfEach());
    expect(!probe.ok);
    expect(probe.layoutMismatch.empty());
}};

// touch_accessed is what makes `cache gc` an LRU rather than "drop what was
// populated long ago". A use sets the time of the entry's record (mcpp::home::
// mark_used, design 2026-10-31 §4.6): the record's content and the artifacts
// stay as they are -- the artifacts' mtimes participate in ninja's restat
// handling -- and a second use within 12 hours writes nothing.
UTest touch_accessed_moves_the_stamp_and_not_the_artifacts{"BmiCache/TouchAccessedMovesTheStampAndNotTheArtifacts", [] {
    Tmp t;
    auto home    = t.path / "home";
    auto project = t.path / "proj" / "target";
    seedProject(project);

    auto k = makeKey(home);
    expect(fatal(bool(populate_from(k, project, oneOfEach()))));

    const auto content0 = readJson(k.entryFile()).dump();
    auto objTime0  = std::filesystem::last_write_time(k.objDir() / "lib.m.o");
    auto bmiTime0  = std::filesystem::last_write_time(k.bmiDir() / "lib.gcm");

    // The record last used two days ago, then a use.
    const auto old = std::filesystem::file_time_type::clock::now() - std::chrono::hours(48);
    std::filesystem::last_write_time(k.entryFile(), old);
    touch_accessed(k);

    const auto used = std::filesystem::last_write_time(k.entryFile());
    expect(gt(used, old + std::chrono::hours(47))) << "a use must move the record's time";
    expect(eq(readJson(k.entryFile()).dump(), content0)) << "a use must not rewrite the record";
    expect(eq(std::filesystem::last_write_time(k.objDir() / "lib.m.o"), objTime0));
    expect(eq(std::filesystem::last_write_time(k.bmiDir() / "lib.gcm"), bmiTime0));
    expect(is_cached(k, oneOfEach())) << "touching must not invalidate the entry";

    // Within 12 hours a second use writes nothing.
    const auto recent = std::filesystem::file_time_type::clock::now() - std::chrono::hours(1);
    std::filesystem::last_write_time(k.entryFile(), recent);
    touch_accessed(k);
    expect(eq(std::filesystem::last_write_time(k.entryFile()), recent));
}};

UTest repopulate_preserves_created_stamp{"BmiCache/RepopulatePreservesCreatedStamp", [] {
    Tmp t;
    auto home    = t.path / "home";
    auto project = t.path / "proj" / "target";
    seedProject(project);

    auto k = makeKey(home);
    expect(fatal(bool(populate_from(k, project, oneOfEach()))));
    auto j0 = readJson(k.entryFile());
    j0["created"] = "12345";
    std::ofstream(k.entryFile()) << j0.dump(2);

    expect(fatal(bool(populate_from(k, project, oneOfEach()))));
    expect(eq(readJson(k.entryFile()).value("created", std::string{}), "12345"));
}};

UTest populate_fails_if_build_output_missing{"BmiCache/PopulateFailsIfBuildOutputMissing", [] {
    Tmp t;
    auto home    = t.path / "home";
    auto project = t.path / "proj" / "target";
    std::filesystem::create_directories(project / "gcm.cache");
    DepArtifacts arts { .bmiFiles = {"missing.gcm"} };
    auto k = makeKey(home);
    auto pop = populate_from(k, project, arts);
    expect(!pop);
    expect(ne(pop.error().find("expected build output missing"), std::string::npos));
}};

// Two keys for the same package@version are independent entries — that is what
// lets one machine hold a dev-profile and a release-profile build of the same
// dependency at once.
UTest different_keys_are_independent_entries{"BmiCache/DifferentKeysAreIndependentEntries", [] {
    Tmp t;
    auto home    = t.path / "home";
    auto project = t.path / "proj" / "target";
    seedProject(project);

    auto a = makeKey(home, "aaaaaaaaaaaaaaaa", "opt");
    auto b = makeKey(home, "bbbbbbbbbbbbbbbb", "debug");
    expect(fatal(bool(populate_from(a, project, oneOfEach()))));
    expect(fatal(bool(populate_from(b, project, oneOfEach()))));
    expect(ne(a.dir(), b.dir()));
    expect(is_cached(a, oneOfEach()));
    expect(is_cached(b, oneOfEach()));
}};

#if !defined(_WIN32)
// When an external holder takes the .lock, populate_from must skip (returns
// success but does NOT clobber the entry). Uses flock(), POSIX-only.
UTest populate_skips_when_lock_held{"BmiCache/PopulateSkipsWhenLockHeld", [] {
    Tmp t;
    auto home    = t.path / "home";
    auto project = t.path / "proj" / "target";
    seedProject(project);

    auto k = makeKey(home);
    std::filesystem::create_directories(k.dir());
    auto lockPath = k.dir() / ".lock";
    int fd = ::open(lockPath.c_str(), O_CREAT | O_RDWR, 0644);
    expect(fatal(ge(fd, 0)));
    expect(fatal(eq(::flock(fd, LOCK_EX | LOCK_NB), 0)));

    auto pop = populate_from(k, project, oneOfEach());
    expect(bool(pop)) << "should silently skip when lock is held";
    expect(!(std::filesystem::exists(k.entryFile())));

    ::flock(fd, LOCK_UN);
    ::close(fd);

    auto pop2 = populate_from(k, project, oneOfEach());
    if (auto&& failed = expect(fatal(bool(pop2))); !failed) failed << pop2.error();
    expect(std::filesystem::exists(k.entryFile()));
}};
#endif // !defined(_WIN32)
