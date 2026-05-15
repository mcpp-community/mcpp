#include <gtest/gtest.h>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

import std;
import mcpp.bmi_cache;

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

CacheKey makeKey(const std::filesystem::path& home) {
    return CacheKey{
        .mcppHome    = home,
        .fingerprint = "deadbeef0123abcd",
        .indexName   = "mcpplibs",
        .packageName = "mcpplibs.cmdline",
        .version     = "0.0.1",
    };
}

void writeFile(const std::filesystem::path& p, std::string_view body) {
    std::filesystem::create_directories(p.parent_path());
    std::ofstream(p) << body;
}

} // namespace

TEST(BmiCache, KeyDirLayoutMatchesDocs26) {
    auto k = makeKey("/home/u/.mcpp");
    EXPECT_EQ(k.dir().string(),
              "/home/u/.mcpp/bmi/deadbeef0123abcd/deps/mcpplibs/mcpplibs.cmdline@0.0.1");
    EXPECT_EQ(k.manifestFile().filename().string(), "manifest.txt");
    EXPECT_EQ(k.bmiDir().filename().string(),       "gcm.cache");
    EXPECT_EQ(k.objDir().filename().string(),       "obj");
}

TEST(BmiCache, IsCachedFalseWhenManifestMissing) {
    Tmp t;
    auto k = makeKey(t.path);
    EXPECT_FALSE(is_cached(k));
}

TEST(BmiCache, PopulateThenStageRoundTrip) {
    Tmp t;
    auto home    = t.path / "home";
    auto project = t.path / "proj" / "target";
    std::filesystem::create_directories(project / "gcm.cache");
    std::filesystem::create_directories(project / "obj");

    writeFile(project / "gcm.cache" / "mcpplibs.cmdline.gcm",         "GCM-A");
    writeFile(project / "gcm.cache" / "mcpplibs.cmdline-options.gcm", "GCM-B");
    writeFile(project / "obj"       / "cmdline.m.o",                  "OBJ-A");

    DepArtifacts arts {
        .bmiFiles = { "mcpplibs.cmdline.gcm", "mcpplibs.cmdline-options.gcm" },
        .objFiles = { "cmdline.m.o" },
    };

    auto k = makeKey(home);
    auto pop = populate_from(k, project, arts);
    ASSERT_TRUE(pop) << pop.error();

    EXPECT_TRUE(std::filesystem::exists(k.manifestFile()));
    EXPECT_TRUE(std::filesystem::exists(k.bmiDir() / "mcpplibs.cmdline.gcm"));
    EXPECT_TRUE(std::filesystem::exists(k.bmiDir() / "mcpplibs.cmdline-options.gcm"));
    EXPECT_TRUE(std::filesystem::exists(k.objDir() / "cmdline.m.o"));
    EXPECT_TRUE(is_cached(k));

    // Round-trip: stage into a fresh project dir.
    auto project2 = t.path / "proj2" / "target";
    auto staged = stage_into(k, project2);
    ASSERT_TRUE(staged) << staged.error();
    EXPECT_EQ(staged->bmiFiles.size(), 2u);
    EXPECT_EQ(staged->objFiles.size(), 1u);
    EXPECT_TRUE(std::filesystem::exists(project2 / "gcm.cache" / "mcpplibs.cmdline.gcm"));
    EXPECT_TRUE(std::filesystem::exists(project2 / "obj"       / "cmdline.m.o"));

    // Staged file content must match original.
    std::ifstream is(project2 / "obj" / "cmdline.m.o");
    std::string body((std::istreambuf_iterator<char>(is)), {});
    EXPECT_EQ(body, "OBJ-A");
}

TEST(BmiCache, StageIntoDoesNotTouchIdenticalOutputs) {
    Tmp t;
    auto home    = t.path / "home";
    auto project = t.path / "proj" / "target";
    std::filesystem::create_directories(project / "gcm.cache");
    std::filesystem::create_directories(project / "obj");

    writeFile(project / "gcm.cache" / "mcpplibs.cmdline.gcm", "GCM-A");
    writeFile(project / "obj"       / "cmdline.m.o",          "OBJ-A");

    DepArtifacts arts {
        .bmiFiles = { "mcpplibs.cmdline.gcm" },
        .objFiles = { "cmdline.m.o" },
    };

    auto k = makeKey(home);
    ASSERT_TRUE(populate_from(k, project, arts));

    auto staged = stage_into(k, project);
    ASSERT_TRUE(staged) << staged.error();
    auto gcmTime = std::filesystem::last_write_time(project / "gcm.cache" / "mcpplibs.cmdline.gcm");
    auto objTime = std::filesystem::last_write_time(project / "obj" / "cmdline.m.o");

    auto stagedAgain = stage_into(k, project);
    ASSERT_TRUE(stagedAgain) << stagedAgain.error();
    EXPECT_EQ(std::filesystem::last_write_time(project / "gcm.cache" / "mcpplibs.cmdline.gcm"), gcmTime);
    EXPECT_EQ(std::filesystem::last_write_time(project / "obj" / "cmdline.m.o"), objTime);
}

TEST(BmiCache, StageIntoDoesNotOverwriteExistingOutputs) {
    Tmp t;
    auto home         = t.path / "home";
    auto cacheProject = t.path / "cache-proj" / "target";
    auto project      = t.path / "proj" / "target";
    std::filesystem::create_directories(cacheProject / "gcm.cache");
    std::filesystem::create_directories(cacheProject / "obj");
    std::filesystem::create_directories(project / "gcm.cache");
    std::filesystem::create_directories(project / "obj");

    writeFile(cacheProject / "gcm.cache" / "mcpplibs.cmdline.gcm", "CACHE-GCM");
    writeFile(cacheProject / "obj"       / "cmdline.m.o",          "CACHE-OBJ");

    DepArtifacts arts {
        .bmiFiles = { "mcpplibs.cmdline.gcm" },
        .objFiles = { "cmdline.m.o" },
    };

    auto k = makeKey(home);
    ASSERT_TRUE(populate_from(k, cacheProject, arts));

    writeFile(project / "gcm.cache" / "mcpplibs.cmdline.gcm", "PROJECT-GCM");
    writeFile(project / "obj"       / "cmdline.m.o",          "PROJECT-OBJ");
    auto gcmTime = std::filesystem::last_write_time(project / "gcm.cache" / "mcpplibs.cmdline.gcm");
    auto objTime = std::filesystem::last_write_time(project / "obj" / "cmdline.m.o");

    auto staged = stage_into(k, project);
    ASSERT_TRUE(staged) << staged.error();

    {
        std::ifstream is(project / "gcm.cache" / "mcpplibs.cmdline.gcm");
        std::string body((std::istreambuf_iterator<char>(is)), {});
        EXPECT_EQ(body, "PROJECT-GCM");
    }
    {
        std::ifstream is(project / "obj" / "cmdline.m.o");
        std::string body((std::istreambuf_iterator<char>(is)), {});
        EXPECT_EQ(body, "PROJECT-OBJ");
    }
    EXPECT_EQ(std::filesystem::last_write_time(project / "gcm.cache" / "mcpplibs.cmdline.gcm"), gcmTime);
    EXPECT_EQ(std::filesystem::last_write_time(project / "obj" / "cmdline.m.o"), objTime);
}

TEST(BmiCache, IsCachedFalseWhenSentinelExistsButFileMissing) {
    Tmp t;
    auto home    = t.path / "home";
    auto project = t.path / "proj" / "target";
    std::filesystem::create_directories(project / "gcm.cache");
    std::filesystem::create_directories(project / "obj");
    writeFile(project / "gcm.cache" / "lib.gcm", "G");
    writeFile(project / "obj"       / "lib.m.o", "O");

    DepArtifacts arts { .bmiFiles = {"lib.gcm"}, .objFiles = {"lib.m.o"} };
    auto k = makeKey(home);
    ASSERT_TRUE(populate_from(k, project, arts));
    ASSERT_TRUE(is_cached(k));

    // Delete one cached file → is_cached must return false even though manifest still exists.
    std::filesystem::remove(k.objDir() / "lib.m.o");
    EXPECT_FALSE(is_cached(k));
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

// M4 #9: when an external holder takes the .lock, populate_from must skip
// (returns success but does NOT clobber the directory).
TEST(BmiCache, PopulateSkipsWhenLockHeld) {
    Tmp t;
    auto home    = t.path / "home";
    auto project = t.path / "proj" / "target";
    std::filesystem::create_directories(project / "gcm.cache");
    std::filesystem::create_directories(project / "obj");
    writeFile(project / "gcm.cache" / "lib.gcm", "G2");
    writeFile(project / "obj"       / "lib.m.o", "O2");

    auto k = makeKey(home);
    // Take the lock manually before populate runs.
    std::filesystem::create_directories(k.dir());
    auto lockPath = k.dir() / ".lock";
    int fd = ::open(lockPath.c_str(), O_CREAT | O_RDWR, 0644);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(::flock(fd, LOCK_EX | LOCK_NB), 0);

    DepArtifacts arts { .bmiFiles = {"lib.gcm"}, .objFiles = {"lib.m.o"} };
    auto pop = populate_from(k, project, arts);
    EXPECT_TRUE(pop) << "should silently skip when lock is held";
    // manifest.txt must NOT have been written by the second writer.
    EXPECT_FALSE(std::filesystem::exists(k.manifestFile()));

    ::flock(fd, LOCK_UN);
    ::close(fd);

    // After lock released, a fresh populate should succeed.
    auto pop2 = populate_from(k, project, arts);
    ASSERT_TRUE(pop2) << pop2.error();
    EXPECT_TRUE(std::filesystem::exists(k.manifestFile()));
}
