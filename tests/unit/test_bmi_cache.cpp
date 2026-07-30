#include <gtest/gtest.h>
#include <fcntl.h>
#if !defined(_WIN32)
#include <sys/file.h>
#include <unistd.h>
#endif

import std;
import mcpp.bmi_cache;
import mcpp.libs.json;

using namespace mcpp::bmi_cache;

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

DepArtifacts oneOfEach() {
    return DepArtifacts{ .bmiFiles = {"lib.gcm"}, .objFiles = {"lib.m.o"} };
}

void seedProject(const std::filesystem::path& project) {
    writeFile(project / "gcm.cache" / "lib.gcm", "G");
    writeFile(project / "obj"       / "lib.m.o", "O");
}

} // namespace

TEST(BmiCache, EntryDirIsKeyedByPackageIdentityAndKeyNotByProject) {
    auto k = makeKey("/home/u/.mcpp/build-cache/v1");
    auto expected = std::filesystem::path(
        "/home/u/.mcpp/build-cache/v1/pkg/mcpplibs/mcpplibs.cmdline@0.0.1/deadbeef0123abcd");
    EXPECT_EQ(k.dir(), expected);
    EXPECT_EQ(k.entryFile().filename().string(), "entry.json");
    EXPECT_EQ(k.bmiDir().filename().string(),    "bmi");
    EXPECT_EQ(k.objDir().filename().string(),    "obj");
}

// The layout version must be a path segment: replacing the layout has to be a
// new tree, never a migration of (or a deletion of) the old one.
TEST(BmiCache, EntryDirCarriesTheLayoutVersion) {
    auto k = makeKey("/home/u/.mcpp/build-cache/v1");
    auto s = k.dir().generic_string();
    EXPECT_NE(s.find("/build-cache/v1/pkg/"), std::string::npos) << s;
    // And nothing may land under the pre-v1 root.
    EXPECT_EQ(s.find("/.mcpp/bmi/"), std::string::npos) << s;
}

TEST(BmiCache, IsCachedFalseWhenEntryMissing) {
    Tmp t;
    EXPECT_FALSE(is_cached(makeKey(t.path)));
}

TEST(BmiCache, PopulateWritesSelfDescribingEntry) {
    Tmp t;
    auto home    = t.path / "home";
    auto project = t.path / "proj" / "target";
    seedProject(project);

    auto k = makeKey(home);
    auto pop = populate_from(k, project, oneOfEach());
    ASSERT_TRUE(pop) << pop.error();

    ASSERT_TRUE(std::filesystem::exists(k.entryFile()));
    EXPECT_TRUE(std::filesystem::exists(k.bmiDir() / "lib.gcm"));
    EXPECT_TRUE(std::filesystem::exists(k.objDir() / "lib.m.o"));
    EXPECT_TRUE(is_cached(k));

    // The entry has to carry the key AND the full inputs: a hit is validated
    // field by field, so a cache whose entries only listed files could never be
    // audited when a wrong hit was suspected.
    auto j = readJson(k.entryFile());
    EXPECT_EQ(j.value("key", std::string{}), "deadbeef0123abcd");
    EXPECT_EQ(j["inputs"], makeInputs());
    EXPECT_EQ(j["bmi"].size(), 1u);
    EXPECT_EQ(j["obj"].size(), 1u);
    EXPECT_TRUE(j.contains("created"));
    EXPECT_TRUE(j.contains("accessed"));
}

// The whole point of recording inputs: equal hashes are never trusted on their
// own. Same key directory, different inputs ⇒ miss.
TEST(BmiCache, IsCachedFalseWhenRecordedInputsDiffer) {
    Tmp t;
    auto home    = t.path / "home";
    auto project = t.path / "proj" / "target";
    seedProject(project);

    auto written = makeKey(home, "samekey00000000", "base");
    ASSERT_TRUE(populate_from(written, project, oneOfEach()));
    EXPECT_TRUE(is_cached(written));

    auto probed = makeKey(home, "samekey00000000", "different-flag");
    EXPECT_FALSE(is_cached(probed));
}

// An entry written by an older mcpp may carry extra keys, but every field the
// current build cares about must be present. Missing ⇒ mismatch, never a pass.
TEST(BmiCache, IsCachedFalseWhenARequiredInputFieldIsAbsent) {
    Tmp t;
    auto home    = t.path / "home";
    auto project = t.path / "proj" / "target";
    seedProject(project);

    auto k = makeKey(home);
    ASSERT_TRUE(populate_from(k, project, oneOfEach()));

    auto j = readJson(k.entryFile());
    j["inputs"].erase("profile");
    std::ofstream(k.entryFile()) << j.dump(2);

    EXPECT_FALSE(is_cached(k));
}

TEST(BmiCache, IsCachedFalseWhenSchemaDiffers) {
    Tmp t;
    auto home    = t.path / "home";
    auto project = t.path / "proj" / "target";
    seedProject(project);

    auto k = makeKey(home);
    ASSERT_TRUE(populate_from(k, project, oneOfEach()));

    auto j = readJson(k.entryFile());
    j["schema"] = kEntrySchema + 1;
    std::ofstream(k.entryFile()) << j.dump(2);

    EXPECT_FALSE(is_cached(k));
}

TEST(BmiCache, IsCachedFalseWhenSentinelExistsButFileMissing) {
    Tmp t;
    auto home    = t.path / "home";
    auto project = t.path / "proj" / "target";
    seedProject(project);

    auto k = makeKey(home);
    ASSERT_TRUE(populate_from(k, project, oneOfEach()));
    ASSERT_TRUE(is_cached(k));

    std::filesystem::remove(k.objDir() / "lib.m.o");
    EXPECT_FALSE(is_cached(k));
}

TEST(BmiCache, ResolveCachedReportsTheArtifactsWithoutCopying) {
    Tmp t;
    auto home    = t.path / "home";
    auto project = t.path / "proj" / "target";
    seedProject(project);

    auto k = makeKey(home);
    ASSERT_TRUE(populate_from(k, project, oneOfEach()));

    auto arts = resolve_cached(k);
    ASSERT_TRUE(arts) << arts.error();
    EXPECT_EQ(arts->bmiFiles, std::vector<std::string>{"lib.gcm"});
    EXPECT_EQ(arts->objFiles, std::vector<std::string>{"lib.m.o"});

    // resolve_cached must not write into a project dir. Staging is a ninja edge
    // now: copying artifacts in from outside the graph is exactly what made the
    // old cache a no-op — ninja rebuilds any output it has no command-line
    // record for, so the copies were recompiled over on every fresh build dir.
    auto project2 = t.path / "proj2" / "target";
    (void)resolve_cached(k);
    EXPECT_FALSE(std::filesystem::exists(project2));
}

TEST(BmiCache, CachedPathsPointIntoTheEntry) {
    auto k = makeKey("/home/u/.mcpp/build-cache/v1");
    EXPECT_EQ(cached_bmi_path(k, "lib.gcm"), k.bmiDir() / "lib.gcm");
    EXPECT_EQ(cached_obj_path(k, "sub/lib.m.o"), k.objDir() / "sub" / "lib.m.o");
}

// Nested object paths (mcpp#233's per-package object prefixes) must round-trip.
TEST(BmiCache, PopulateHandlesNestedObjectPaths) {
    Tmp t;
    auto home    = t.path / "home";
    auto project = t.path / "proj" / "target";
    writeFile(project / "obj" / "pkg_zlib" / "zlib-1.3" / "compress.o", "NESTED");

    auto k = makeKey(home);
    DepArtifacts arts { .objFiles = {"pkg_zlib/zlib-1.3/compress.o"} };
    ASSERT_TRUE(populate_from(k, project, arts));
    EXPECT_EQ(readFile(k.objDir() / "pkg_zlib" / "zlib-1.3" / "compress.o"), "NESTED");
    EXPECT_TRUE(is_cached(k));
}

// touch_accessed is what makes `cache gc` an LRU rather than "drop what was
// populated long ago". It must move the stamp and leave the artifacts alone —
// their mtimes participate in ninja's restat handling.
TEST(BmiCache, TouchAccessedMovesTheStampAndNotTheArtifacts) {
    Tmp t;
    auto home    = t.path / "home";
    auto project = t.path / "proj" / "target";
    seedProject(project);

    auto k = makeKey(home);
    ASSERT_TRUE(populate_from(k, project, oneOfEach()));

    auto j0 = readJson(k.entryFile());
    auto created0  = j0.value("created", std::string{});
    auto objTime0  = std::filesystem::last_write_time(k.objDir() / "lib.m.o");
    auto bmiTime0  = std::filesystem::last_write_time(k.bmiDir() / "lib.gcm");

    // Rewrite the stamp to something clearly old, then touch.
    j0["accessed"] = "1000";
    std::ofstream(k.entryFile()) << j0.dump(2);
    touch_accessed(k);

    auto j1 = readJson(k.entryFile());
    EXPECT_NE(j1.value("accessed", std::string{}), "1000");
    EXPECT_EQ(j1.value("created", std::string{}), created0)
        << "touch must not reset the creation stamp";
    EXPECT_EQ(std::filesystem::last_write_time(k.objDir() / "lib.m.o"), objTime0);
    EXPECT_EQ(std::filesystem::last_write_time(k.bmiDir() / "lib.gcm"), bmiTime0);
    EXPECT_TRUE(is_cached(k)) << "touching must not invalidate the entry";
}

TEST(BmiCache, RepopulatePreservesCreatedStamp) {
    Tmp t;
    auto home    = t.path / "home";
    auto project = t.path / "proj" / "target";
    seedProject(project);

    auto k = makeKey(home);
    ASSERT_TRUE(populate_from(k, project, oneOfEach()));
    auto j0 = readJson(k.entryFile());
    j0["created"] = "12345";
    std::ofstream(k.entryFile()) << j0.dump(2);

    ASSERT_TRUE(populate_from(k, project, oneOfEach()));
    EXPECT_EQ(readJson(k.entryFile()).value("created", std::string{}), "12345");
}

TEST(BmiCache, PopulateFailsIfBuildOutputMissing) {
    Tmp t;
    auto home    = t.path / "home";
    auto project = t.path / "proj" / "target";
    std::filesystem::create_directories(project / "gcm.cache");
    DepArtifacts arts { .bmiFiles = {"missing.gcm"}, .objFiles = {} };
    auto k = makeKey(home);
    auto pop = populate_from(k, project, arts);
    EXPECT_FALSE(pop);
    EXPECT_NE(pop.error().find("expected build output missing"), std::string::npos);
}

// Two keys for the same package@version are independent entries — that is what
// lets one machine hold a dev-profile and a release-profile build of the same
// dependency at once.
TEST(BmiCache, DifferentKeysAreIndependentEntries) {
    Tmp t;
    auto home    = t.path / "home";
    auto project = t.path / "proj" / "target";
    seedProject(project);

    auto a = makeKey(home, "aaaaaaaaaaaaaaaa", "opt");
    auto b = makeKey(home, "bbbbbbbbbbbbbbbb", "debug");
    ASSERT_TRUE(populate_from(a, project, oneOfEach()));
    ASSERT_TRUE(populate_from(b, project, oneOfEach()));
    EXPECT_NE(a.dir(), b.dir());
    EXPECT_TRUE(is_cached(a));
    EXPECT_TRUE(is_cached(b));
}

#if !defined(_WIN32)
// When an external holder takes the .lock, populate_from must skip (returns
// success but does NOT clobber the entry). Uses flock(), POSIX-only.
TEST(BmiCache, PopulateSkipsWhenLockHeld) {
    Tmp t;
    auto home    = t.path / "home";
    auto project = t.path / "proj" / "target";
    seedProject(project);

    auto k = makeKey(home);
    std::filesystem::create_directories(k.dir());
    auto lockPath = k.dir() / ".lock";
    int fd = ::open(lockPath.c_str(), O_CREAT | O_RDWR, 0644);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(::flock(fd, LOCK_EX | LOCK_NB), 0);

    auto pop = populate_from(k, project, oneOfEach());
    EXPECT_TRUE(pop) << "should silently skip when lock is held";
    EXPECT_FALSE(std::filesystem::exists(k.entryFile()));

    ::flock(fd, LOCK_UN);
    ::close(fd);

    auto pop2 = populate_from(k, project, oneOfEach());
    ASSERT_TRUE(pop2) << pop2.error();
    EXPECT_TRUE(std::filesystem::exists(k.entryFile()));
}
#endif // !defined(_WIN32)
