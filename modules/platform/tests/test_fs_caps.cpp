#include <gtest/gtest.h>
#if !defined(_WIN32)
#include <sys/stat.h>
#include <unistd.h>
#endif

import std;
import mcpp.platform.fs_caps;

// What a filesystem does, and the strategy each answer selects (#809). The
// strategies are driven by stated capabilities, so every one of them runs on
// whatever filesystem the test machine has -- the NFSv3 run is the CI lane's.

namespace fs = std::filesystem;
namespace pf = mcpp::platform;

namespace {
struct Scratch {
    fs::path dir;
    Scratch() {
        std::random_device random;
        dir = fs::temp_directory_path() / std::format("mcpp-fscaps-{:08x}", random());
        fs::create_directories(dir);
    }
    ~Scratch() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};
void write(const fs::path& p, std::string_view text) { std::ofstream(p) << text; }
std::string read(const fs::path& p) {
    std::ifstream in(p);
    return {std::istreambuf_iterator<char>(in), {}};
}
}  // namespace

TEST(PlatformFsCaps, TheProbeMeasuresAWritableDirectoryAndLeavesNothing) {
    Scratch s;
    const auto caps = pf::probe_fs_caps(s.dir);
    EXPECT_TRUE(caps.measured);
    EXPECT_TRUE(fs::is_empty(s.dir)) << "the probe removes its scratch";
    // A directory that cannot be written: the platform's native answers.
    const auto none = pf::probe_fs_caps(s.dir / "absent");
    EXPECT_FALSE(none.measured);
    EXPECT_TRUE(none.rename_no_replace);
}

TEST(PlatformFsCaps, EveryStrategyPublishesAndNoneReplacesAName) {
    Scratch s;
    for (int strategy = 0; strategy < 3; ++strategy) {
        pf::FsCaps caps;
        caps.rename_no_replace = strategy == 0;
        caps.hard_links = strategy <= 1;
        const auto dir = s.dir / std::to_string(strategy);
        fs::create_directories(dir);
        // A file.
        write(dir / "scratch", "new");
        ASSERT_TRUE(pf::publish_no_replace(dir / "scratch", dir / "file", caps)) << strategy;
        EXPECT_EQ(read(dir / "file"), "new");
        EXPECT_FALSE(fs::exists(dir / "scratch"));
        write(dir / "scratch", "newer");
        EXPECT_FALSE(pf::publish_no_replace(dir / "scratch", dir / "file", caps)) << strategy;
        EXPECT_EQ(read(dir / "file"), "new") << "an existing name is never replaced";
        // A directory.
        fs::create_directory(dir / "staged");
        write(dir / "staged" / "a", "a");
        ASSERT_TRUE(pf::publish_no_replace(dir / "staged", dir / "tree", caps)) << strategy;
        EXPECT_EQ(read(dir / "tree" / "a"), "a");
        fs::create_directory(dir / "staged");
        EXPECT_FALSE(pf::publish_no_replace(dir / "staged", dir / "tree", caps));
        // A symbolic link (where the filesystem has them).
        std::error_code ec;
        fs::create_symlink("file", dir / "link-scratch", ec);
        if (!ec) {
            ASSERT_TRUE(pf::publish_no_replace(dir / "link-scratch", dir / "link", caps)) << strategy;
            EXPECT_EQ(fs::read_symlink(dir / "link"), fs::path("file"));
        }
    }
}

TEST(PlatformFsCaps, AnExchangeTradesTwoTreesWhateverTheFilesystemDoes) {
    Scratch s;
    fs::create_directories(s.dir / "next");
    fs::create_directories(s.dir / "live");
    write(s.dir / "next" / "rev", "2");
    write(s.dir / "live" / "rev", "1");
    ASSERT_TRUE(pf::exchange_paths(s.dir / "next", s.dir / "live"));
    EXPECT_EQ(read(s.dir / "live" / "rev"), "2");
    EXPECT_EQ(read(s.dir / "next" / "rev"), "1");
    std::size_t entries = 0;
    for ([[maybe_unused]] const auto& e : fs::directory_iterator(s.dir)) ++entries;
    EXPECT_EQ(entries, 2u) << "nothing set aside is left";
}

// Whom an upstream home may belong to (design 2026-10-16 §8.2): this user, or
// the system when this user cannot change it.
TEST(PlatformTrust, ThisUsersDirectoryIsTrusted) {
    Scratch s;
    EXPECT_EQ(pf::untrusted_because(s.dir), "");
    EXPECT_NE(pf::untrusted_because(s.dir / "absent"), "");
}

#if !defined(_WIN32)
TEST(PlatformTrust, ASystemDirectoryThisUserCanWriteIsNot) {
    if (::getuid() == 0) GTEST_SKIP() << "root may write anything";
    const auto rootOwned = [](const char* p) {
        struct stat st {};
        return ::stat(p, &st) == 0 && st.st_uid == 0;
    };
    // /tmp: root's, and writable by everyone.
    if (rootOwned("/tmp") && ::access("/tmp", W_OK) == 0) EXPECT_NE(pf::untrusted_because("/tmp"), "");
    // /usr: root's, and not this user's to write.
    if (rootOwned("/usr") && ::access("/usr", W_OK) != 0) EXPECT_EQ(pf::untrusted_because("/usr"), "");
}
#endif

TEST(PlatformTrust, ADirectoryLinkReachesItsTarget) {
    Scratch s;
    fs::create_directories(s.dir / "target");
    write(s.dir / "target" / "f", "x");
    ASSERT_TRUE(pf::create_directory_link(s.dir / "target", s.dir / "link"));
    EXPECT_EQ(read(s.dir / "link" / "f"), "x");
    // Removing the link removes the link only.
    std::error_code ec;
    fs::remove(s.dir / "link", ec);
    EXPECT_TRUE(fs::exists(s.dir / "target" / "f"));
}
