#include <gtest/gtest.h>

import std;
import mcpp.fallback.install_integrity;

// Regression coverage for InstallStash — the rollback-safe replacement for a
// bare clean_incomplete_install() on the resolve/install path.
//
// Background: a content-complete *legacy* package (no .mcpp_ok marker) used to
// be deleted outright before a reinstall. If the reinstall then failed, the
// working package was gone for good (the xim-x-zlib / clang `libz.so.1` 127
// regression). InstallStash renames it aside and restores it on failure.

namespace {

namespace fs = std::filesystem;

fs::path make_tempdir(std::string_view name) {
    auto base = fs::temp_directory_path()
              / std::format("{}-{}", name,
                            std::chrono::steady_clock::now()
                                .time_since_epoch().count());
    fs::create_directories(base);
    return base;
}

void write_file(const fs::path& p, std::string_view content) {
    fs::create_directories(p.parent_path());
    std::ofstream os(p);
    os << content;
}

// A directory that looks_complete_legacy(): top-level lib/ with a payload.
void make_legacy_pkg(const fs::path& verdir, std::string_view marker) {
    write_file(verdir / "lib" / "libz.so.1", marker);
}

bool has_ok_marker(const fs::path& verdir) {
    return fs::exists(verdir / ".mcpp_ok");
}

// Read the first line and CLOSE the handle before returning. On Windows an
// open file handle blocks fs::remove_all() ("being used by another process"),
// so readers must not outlive the read.
std::string read_first_line(const fs::path& p) {
    std::ifstream in(p);
    std::string line;
    std::getline(in, line);
    return line;
}

}  // namespace

// Reinstall fails (no commit) → a content-complete legacy package is rolled
// back AND adopted (marker written) so the next resolve trusts it.
TEST(InstallIntegrityStash, RestoresLegacyPackageOnFailedReinstall) {
    auto root = make_tempdir("mcpp-stash-restore");
    auto verdir = root / "compat-x-compat.zlib" / "1.3.1";
    make_legacy_pkg(verdir, "ORIGINAL");

    {
        mcpp::fallback::InstallStash stash(verdir);
        EXPECT_TRUE(stash.stashed());
        // verdir was moved aside, leaving the live path clean for a reinstall.
        EXPECT_FALSE(fs::exists(verdir));
        // ... reinstall attempt fails here: no commit() ...
    }  // destructor runs

    // Original content is back, and it is now marked complete.
    ASSERT_TRUE(fs::exists(verdir / "lib" / "libz.so.1"));
    EXPECT_EQ(read_first_line(verdir / "lib" / "libz.so.1"), "ORIGINAL");
    EXPECT_TRUE(has_ok_marker(verdir));
    EXPECT_FALSE(fs::exists(fs::path(verdir.string() + ".mcpp-stash")));

    fs::remove_all(root);
}

// commit() (reinstall produced a good install) → backup is dropped, the live
// directory is left exactly as the reinstall produced it.
TEST(InstallIntegrityStash, CommitDropsBackupAndKeepsNewInstall) {
    auto root = make_tempdir("mcpp-stash-commit");
    auto verdir = root / "compat-x-compat.zlib" / "1.3.1";
    make_legacy_pkg(verdir, "ORIGINAL");

    {
        mcpp::fallback::InstallStash stash(verdir);
        ASSERT_TRUE(stash.stashed());
        // Simulate a successful reinstall landing fresh content.
        write_file(verdir / "lib" / "libz.so.1", "REINSTALLED");
        stash.commit();
    }

    ASSERT_TRUE(fs::exists(verdir / "lib" / "libz.so.1"));
    EXPECT_EQ(read_first_line(verdir / "lib" / "libz.so.1"), "REINSTALLED");
    EXPECT_FALSE(fs::exists(fs::path(verdir.string() + ".mcpp-stash")));

    fs::remove_all(root);
}

// No commit, but the reinstall DID produce a complete install (marker present)
// → keep the new install, discard the stash (don't clobber a good result).
TEST(InstallIntegrityStash, KeepsNewCompleteInstallWhenUncommitted) {
    auto root = make_tempdir("mcpp-stash-newcomplete");
    auto verdir = root / "compat-x-compat.zlib" / "1.3.1";
    make_legacy_pkg(verdir, "ORIGINAL");

    {
        mcpp::fallback::InstallStash stash(verdir);
        ASSERT_TRUE(stash.stashed());
        // Reinstall produced a marked-complete install but commit() was missed.
        write_file(verdir / "lib" / "libz.so.1", "REINSTALLED");
        mcpp::fallback::mark_install_complete(verdir);
    }  // destructor: live dir is complete → keep it

    EXPECT_EQ(read_first_line(verdir / "lib" / "libz.so.1"), "REINSTALLED");
    EXPECT_FALSE(fs::exists(fs::path(verdir.string() + ".mcpp-stash")));

    fs::remove_all(root);
}

// Genuine half-extracted residue (NOT looks_complete_legacy) and a failed
// reinstall → discard it, matching the historical delete semantics.
TEST(InstallIntegrityStash, DiscardsNonLegacyResidueOnFailure) {
    auto root = make_tempdir("mcpp-stash-residue");
    auto verdir = root / "compat-x-compat.zlib" / "1.3.1";
    write_file(verdir / "partial.tmp", "half-extracted");  // no bin/lib/... → not legacy-complete

    {
        mcpp::fallback::InstallStash stash(verdir);
        ASSERT_TRUE(stash.stashed());
        // reinstall fails, no commit
    }

    EXPECT_FALSE(fs::exists(verdir));  // residue discarded, not restored
    EXPECT_FALSE(fs::exists(fs::path(verdir.string() + ".mcpp-stash")));

    fs::remove_all(root);
}

// A directory that already has the .mcpp_ok marker must not be stashed.
TEST(InstallIntegrityStash, NoopWhenAlreadyComplete) {
    auto root = make_tempdir("mcpp-stash-complete");
    auto verdir = root / "compat-x-compat.zlib" / "1.3.1";
    make_legacy_pkg(verdir, "ORIGINAL");
    mcpp::fallback::mark_install_complete(verdir);

    {
        mcpp::fallback::InstallStash stash(verdir);
        EXPECT_FALSE(stash.stashed());
        EXPECT_TRUE(fs::exists(verdir / "lib" / "libz.so.1"));  // untouched
    }

    EXPECT_TRUE(fs::exists(verdir / "lib" / "libz.so.1"));
    EXPECT_TRUE(has_ok_marker(verdir));

    fs::remove_all(root);
}
