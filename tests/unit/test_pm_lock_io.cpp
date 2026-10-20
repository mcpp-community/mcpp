#include <gtest/gtest.h>

import std;
import mcpp.pm.lock_io;

TEST(PmLockIo, ParseGitBranchWithCommit) {
    auto parsed = mcpp::pm::parse_git_source(
        "git+https://github.com/user/repo#branch=develop@584894315b7a4fe4d7957d3c29dc4052b8012860");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->url, "https://github.com/user/repo");
    EXPECT_EQ(parsed->refKind, "branch");
    EXPECT_EQ(parsed->ref, "develop");
    ASSERT_TRUE(parsed->resolvedCommit.has_value());
    EXPECT_EQ(parsed->resolvedCommit.value(),
              "584894315b7a4fe4d7957d3c29dc4052b8012860");
}

TEST(PmLockIo, ParseGitBranchWithoutCommit) {
    auto parsed = mcpp::pm::parse_git_source(
        "git+https://github.com/user/repo#branch=develop");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->url, "https://github.com/user/repo");
    EXPECT_EQ(parsed->refKind, "branch");
    EXPECT_EQ(parsed->ref, "develop");
    EXPECT_FALSE(parsed->resolvedCommit.has_value());
}

TEST(PmLockIo, ParseGitTag) {
    auto parsed = mcpp::pm::parse_git_source(
        "git+https://github.com/user/repo#tag=v1.0.0");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->url, "https://github.com/user/repo");
    EXPECT_EQ(parsed->refKind, "tag");
    EXPECT_EQ(parsed->ref, "v1.0.0");
    EXPECT_FALSE(parsed->resolvedCommit.has_value());
}

TEST(PmLockIo, ParseGitRev) {
    auto parsed = mcpp::pm::parse_git_source(
        "git+https://github.com/user/repo#rev=584894315b7a4fe4d7957d3c29dc4052b8012860");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->url, "https://github.com/user/repo");
    EXPECT_EQ(parsed->refKind, "rev");
    EXPECT_EQ(parsed->ref, "584894315b7a4fe4d7957d3c29dc4052b8012860");
    EXPECT_FALSE(parsed->resolvedCommit.has_value());
}

// A branch name may legitimately contain '@' — the commit is whatever follows
// the LAST one, matching how prepare.cppm writes `ref + "@" + commit`.
TEST(PmLockIo, ParseGitBranchNameContainingAt) {
    auto parsed = mcpp::pm::parse_git_source(
        "git+https://github.com/user/repo#branch=feat@v2@584894315b7a4fe4d7957d3c29dc4052b8012860");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->ref, "feat@v2");
    ASSERT_TRUE(parsed->resolvedCommit.has_value());
    EXPECT_EQ(parsed->resolvedCommit.value(),
              "584894315b7a4fe4d7957d3c29dc4052b8012860");
}

// `branch=foo@` records no commit; an empty string would make the anchor claim
// a pin it does not have.
TEST(PmLockIo, ParseGitBranchWithEmptyCommit) {
    auto parsed = mcpp::pm::parse_git_source(
        "git+https://github.com/user/repo#branch=develop@");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->ref, "develop");
    EXPECT_FALSE(parsed->resolvedCommit.has_value());
}

// Only branch entries carry `@<commit>`, so a tag or rev whose name contains
// '@' must survive verbatim rather than be split into ref + commit.
TEST(PmLockIo, ParseGitTagNameContainingAtIsNotSplit) {
    auto tag = mcpp::pm::parse_git_source(
        "git+https://github.com/user/repo#tag=v1.0@rc1");
    ASSERT_TRUE(tag.has_value());
    EXPECT_EQ(tag->ref, "v1.0@rc1");
    EXPECT_FALSE(tag->resolvedCommit.has_value());

    auto rev = mcpp::pm::parse_git_source(
        "git+https://github.com/user/repo#rev=abc123@def");
    ASSERT_TRUE(rev.has_value());
    EXPECT_EQ(rev->ref, "abc123@def");
    EXPECT_FALSE(rev->resolvedCommit.has_value());
}

// An scp-like URL puts '@' before the '#', so the URL half must not be
// disturbed by the ref-side split.
TEST(PmLockIo, ParseGitScpLikeUrl) {
    auto parsed = mcpp::pm::parse_git_source(
        "git+git@github.com:user/repo.git#branch=develop@584894315b7a4fe4d7957d3c29dc4052b8012860");
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->url, "git@github.com:user/repo.git");
    EXPECT_EQ(parsed->ref, "develop");
    ASSERT_TRUE(parsed->resolvedCommit.has_value());
    EXPECT_EQ(parsed->resolvedCommit.value(),
              "584894315b7a4fe4d7957d3c29dc4052b8012860");
}

TEST(PmLockIo, ParseNonGitSourceReturnsNullopt) {
    EXPECT_FALSE(mcpp::pm::parse_git_source("index+mcpplibs@1.0.0").has_value());
    EXPECT_FALSE(mcpp::pm::parse_git_source("").has_value());
    EXPECT_FALSE(
        mcpp::pm::parse_git_source("https://github.com/user/repo").has_value());
    EXPECT_FALSE(mcpp::pm::parse_git_source("git+https://host/repo").has_value());
    EXPECT_FALSE(
        mcpp::pm::parse_git_source("git+https://host/repo#bad").has_value());
}

// The lock digest MUST be the same on every host. It used `std::hash<std::string>`,
// whose output is implementation-defined — MSVC implements it as FNV-1a, while
// libstdc++/libc++ use MurmurHash. The same `compat.catch2` therefore landed in
// mcpp.lock as 50719400df192025 on Windows and f492c0206481c69a on Linux, so a
// clean checkout showed a lock diff after every command, and the `fnv1a:` prefix
// was only true on MSVC. These vectors are the cross-platform contract: a
// host-dependent hash turns them red on one of the two platforms.
TEST(PmLockIo, IndexPackageDigestIsFnv1aOnEveryHost) {
    EXPECT_EQ(mcpp::pm::index_package_digest("compat", "compat.catch2", "2.13.10"),
              "fnv1a:50719400df192025");
    EXPECT_EQ(mcpp::pm::index_package_digest("acme", "acme.gadget", "2.1.0"),
              "fnv1a:2adea846f70078bc");
}


namespace {
struct LockDirectory {
    std::filesystem::path root = std::filesystem::temp_directory_path()
        / std::format("mcpp_lock_write_{}", std::random_device{}());
    LockDirectory() { std::filesystem::create_directories(root); }
    ~LockDirectory() {
        std::error_code ec;
        std::filesystem::permissions(root / "mcpp.lock", std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::add, ec);
        std::filesystem::remove_all(root, ec);
    }
};
std::string lock_bytes(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}
}

TEST(PmLockIo, UnchangedLfAndCrlfLocksKeepTheirBytesAndTimestamp) {
    LockDirectory t;
    const auto file = t.root / "mcpp.lock";
    mcpp::pm::Lockfile lock;
    lock.packages.push_back({"fixture", "example", "1.0.0", "path", "digest"});
    const auto text = mcpp::pm::serialize(lock);
    for (bool crlf : {false, true}) {
        std::string bytes;
        for (char c : text) {
            if (crlf && c == '\n') bytes += '\r';
            bytes += c;
        }
        { std::ofstream out(file, std::ios::binary); out << bytes; }
        const auto old = std::filesystem::file_time_type::clock::now() - std::chrono::hours(24);
        std::filesystem::last_write_time(file, old);
        const auto recorded = std::filesystem::last_write_time(file);
        ASSERT_TRUE(mcpp::pm::write(lock, file));
        EXPECT_EQ(lock_bytes(file), bytes);
        EXPECT_EQ(std::filesystem::last_write_time(file), recorded);
    }
}

TEST(PmLockIo, CreatesMissingParentsAndPublishesChangedResolution) {
    LockDirectory t;
    const auto file = t.root / "nested" / "mcpp.lock";
    mcpp::pm::Lockfile lock;
    lock.packages.push_back({"fixture", "example", "1.0.0", "path", "digest"});
    ASSERT_TRUE(mcpp::pm::write(lock, file));
    lock.packages[0].version = "2.0.0";
    ASSERT_TRUE(mcpp::pm::write(lock, file));
    auto loaded = mcpp::pm::load(file);
    ASSERT_TRUE(loaded);
    ASSERT_EQ(loaded->packages.size(), 1u);
    EXPECT_EQ(loaded->packages[0].version, "2.0.0");
}

TEST(PmLockIo, UnchangedReadOnlyLockDoesNotNeedWriteAccess) {
    LockDirectory t;
    const auto file = t.root / "mcpp.lock";
    mcpp::pm::Lockfile lock;
    ASSERT_TRUE(mcpp::pm::write(lock, file));
    std::filesystem::permissions(file, std::filesystem::perms::owner_read);
    const auto recorded = std::filesystem::last_write_time(file);
    EXPECT_TRUE(mcpp::pm::write(lock, file));
    EXPECT_EQ(std::filesystem::last_write_time(file), recorded);
}

TEST(PmLockIo, AnUnreadableFileOrUncreatableParentReturnsAnError) {
    LockDirectory t;
    const auto blocker = t.root / "file";
    { std::ofstream out(blocker); out << "not a directory"; }
    mcpp::pm::Lockfile lock;
    EXPECT_FALSE(mcpp::pm::write(lock, blocker / "mcpp.lock"));
    EXPECT_FALSE(mcpp::pm::write(lock, t.root));
}


TEST(PmLockIo, UpdatingALinkedLockKeepsTheLinkAndUpdatesItsTarget) {
    LockDirectory t;
    const auto target = t.root / "target.lock";
    const auto link = t.root / "mcpp.lock";
    mcpp::pm::Lockfile lock;
    ASSERT_TRUE(mcpp::pm::write(lock, target));
    std::error_code ec;
    std::filesystem::create_symlink(target, link, ec);
    if (ec) GTEST_SKIP() << "this host does not permit file symlinks: " << ec.message();
    lock.packages.push_back({"fixture", "example", "2.0.0", "path", "digest"});
    ASSERT_TRUE(mcpp::pm::write(lock, link));
    EXPECT_TRUE(std::filesystem::is_symlink(link));
    auto loaded = mcpp::pm::load(target);
    ASSERT_TRUE(loaded);
    ASSERT_EQ(loaded->packages.size(), 1u);
    EXPECT_EQ(loaded->packages[0].version, "2.0.0");
}
