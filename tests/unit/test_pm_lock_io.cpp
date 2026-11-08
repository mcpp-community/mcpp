
import std;
import mcpplibs.mtest;
import mcpp.pm.lock_io;

using namespace mcpplibs::mtest;

UTest parse_git_branch_with_commit{"PmLockIo/ParseGitBranchWithCommit", [] {
    auto parsed = mcpp::pm::parse_git_source(
        "git+https://github.com/user/repo#branch=develop@584894315b7a4fe4d7957d3c29dc4052b8012860");
    expect(fatal(parsed.has_value()));
    expect(eq(parsed->url, "https://github.com/user/repo"));
    expect(eq(parsed->refKind, "branch"));
    expect(eq(parsed->ref, "develop"));
    expect(fatal(parsed->resolvedCommit.has_value()));
    expect(eq(parsed->resolvedCommit.value(), "584894315b7a4fe4d7957d3c29dc4052b8012860"));
}};

UTest parse_git_branch_without_commit{"PmLockIo/ParseGitBranchWithoutCommit", [] {
    auto parsed = mcpp::pm::parse_git_source(
        "git+https://github.com/user/repo#branch=develop");
    expect(fatal(parsed.has_value()));
    expect(eq(parsed->url, "https://github.com/user/repo"));
    expect(eq(parsed->refKind, "branch"));
    expect(eq(parsed->ref, "develop"));
    expect(!(parsed->resolvedCommit.has_value()));
}};

UTest parse_git_tag{"PmLockIo/ParseGitTag", [] {
    auto parsed = mcpp::pm::parse_git_source(
        "git+https://github.com/user/repo#tag=v1.0.0");
    expect(fatal(parsed.has_value()));
    expect(eq(parsed->url, "https://github.com/user/repo"));
    expect(eq(parsed->refKind, "tag"));
    expect(eq(parsed->ref, "v1.0.0"));
    expect(!(parsed->resolvedCommit.has_value()));
}};

UTest parse_git_rev{"PmLockIo/ParseGitRev", [] {
    auto parsed = mcpp::pm::parse_git_source(
        "git+https://github.com/user/repo#rev=584894315b7a4fe4d7957d3c29dc4052b8012860");
    expect(fatal(parsed.has_value()));
    expect(eq(parsed->url, "https://github.com/user/repo"));
    expect(eq(parsed->refKind, "rev"));
    expect(eq(parsed->ref, "584894315b7a4fe4d7957d3c29dc4052b8012860"));
    expect(!(parsed->resolvedCommit.has_value()));
}};

// A branch name may legitimately contain '@' — the commit is whatever follows
// the LAST one, matching how prepare.cppm writes `ref + "@" + commit`.
UTest parse_git_branch_name_containing_at{"PmLockIo/ParseGitBranchNameContainingAt", [] {
    auto parsed = mcpp::pm::parse_git_source(
        "git+https://github.com/user/repo#branch=feat@v2@584894315b7a4fe4d7957d3c29dc4052b8012860");
    expect(fatal(parsed.has_value()));
    expect(eq(parsed->ref, "feat@v2"));
    expect(fatal(parsed->resolvedCommit.has_value()));
    expect(eq(parsed->resolvedCommit.value(), "584894315b7a4fe4d7957d3c29dc4052b8012860"));
}};

// `branch=foo@` records no commit; an empty string would make the anchor claim
// a pin it does not have.
UTest parse_git_branch_with_empty_commit{"PmLockIo/ParseGitBranchWithEmptyCommit", [] {
    auto parsed = mcpp::pm::parse_git_source(
        "git+https://github.com/user/repo#branch=develop@");
    expect(fatal(parsed.has_value()));
    expect(eq(parsed->ref, "develop"));
    expect(!(parsed->resolvedCommit.has_value()));
}};

// Only branch entries carry `@<commit>`, so a tag or rev whose name contains
// '@' must survive verbatim rather than be split into ref + commit.
UTest parse_git_tag_name_containing_at_is_not_split{"PmLockIo/ParseGitTagNameContainingAtIsNotSplit", [] {
    auto tag = mcpp::pm::parse_git_source(
        "git+https://github.com/user/repo#tag=v1.0@rc1");
    expect(fatal(tag.has_value()));
    expect(eq(tag->ref, "v1.0@rc1"));
    expect(!(tag->resolvedCommit.has_value()));

    auto rev = mcpp::pm::parse_git_source(
        "git+https://github.com/user/repo#rev=abc123@def");
    expect(fatal(rev.has_value()));
    expect(eq(rev->ref, "abc123@def"));
    expect(!(rev->resolvedCommit.has_value()));
}};

// An scp-like URL puts '@' before the '#', so the URL half must not be
// disturbed by the ref-side split.
UTest parse_git_scp_like_url{"PmLockIo/ParseGitScpLikeUrl", [] {
    auto parsed = mcpp::pm::parse_git_source(
        "git+git@github.com:user/repo.git#branch=develop@584894315b7a4fe4d7957d3c29dc4052b8012860");
    expect(fatal(parsed.has_value()));
    expect(eq(parsed->url, "git@github.com:user/repo.git"));
    expect(eq(parsed->ref, "develop"));
    expect(fatal(parsed->resolvedCommit.has_value()));
    expect(eq(parsed->resolvedCommit.value(), "584894315b7a4fe4d7957d3c29dc4052b8012860"));
}};

UTest parse_non_git_source_returns_nullopt{"PmLockIo/ParseNonGitSourceReturnsNullopt", [] {
    expect(!(mcpp::pm::parse_git_source("index+mcpplibs@1.0.0").has_value()));
    expect(!(mcpp::pm::parse_git_source("").has_value()));
    expect(!(mcpp::pm::parse_git_source("https://github.com/user/repo").has_value()));
    expect(!(mcpp::pm::parse_git_source("git+https://host/repo").has_value()));
    expect(!(mcpp::pm::parse_git_source("git+https://host/repo#bad").has_value()));
}};

// The lock digest MUST be the same on every host. It used `std::hash<std::string>`,
// whose output is implementation-defined — MSVC implements it as FNV-1a, while
// libstdc++/libc++ use MurmurHash. The same `compat.catch2` therefore landed in
// mcpp.lock as 50719400df192025 on Windows and f492c0206481c69a on Linux, so a
// clean checkout showed a lock diff after every command, and the `fnv1a:` prefix
// was only true on MSVC. These vectors are the cross-platform contract: a
// host-dependent hash turns them red on one of the two platforms.
UTest index_package_digest_is_fnv1a_on_every_host{"PmLockIo/IndexPackageDigestIsFnv1aOnEveryHost", [] {
    expect(eq(mcpp::pm::index_package_digest("compat", "compat.catch2", "2.13.10"), "fnv1a:50719400df192025"));
    expect(eq(mcpp::pm::index_package_digest("acme", "acme.gadget", "2.1.0"), "fnv1a:2adea846f70078bc"));
}};


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

UTest unchanged_lf_and_crlf_locks_keep_their_bytes_and_timestamp{"PmLockIo/UnchangedLfAndCrlfLocksKeepTheirBytesAndTimestamp", [] {
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
        expect(fatal(bool(mcpp::pm::write(lock, file))));
        expect(eq(lock_bytes(file), bytes));
        expect(eq(std::filesystem::last_write_time(file), recorded));
    }
}};

UTest creates_missing_parents_and_publishes_changed_resolution{"PmLockIo/CreatesMissingParentsAndPublishesChangedResolution", [] {
    LockDirectory t;
    const auto file = t.root / "nested" / "mcpp.lock";
    mcpp::pm::Lockfile lock;
    lock.packages.push_back({"fixture", "example", "1.0.0", "path", "digest"});
    expect(fatal(bool(mcpp::pm::write(lock, file))));
    lock.packages[0].version = "2.0.0";
    expect(fatal(bool(mcpp::pm::write(lock, file))));
    auto loaded = mcpp::pm::load(file);
    expect(fatal(bool(loaded)));
    expect(fatal(eq(loaded->packages.size(), 1u)));
    expect(eq(loaded->packages[0].version, "2.0.0"));
}};

UTest unchanged_read_only_lock_does_not_need_write_access{"PmLockIo/UnchangedReadOnlyLockDoesNotNeedWriteAccess", [] {
    LockDirectory t;
    const auto file = t.root / "mcpp.lock";
    mcpp::pm::Lockfile lock;
    expect(fatal(bool(mcpp::pm::write(lock, file))));
    std::filesystem::permissions(file, std::filesystem::perms::owner_read);
    const auto recorded = std::filesystem::last_write_time(file);
    expect(bool(mcpp::pm::write(lock, file)));
    expect(eq(std::filesystem::last_write_time(file), recorded));
}};

UTest an_unreadable_file_or_uncreatable_parent_returns_an_error{"PmLockIo/AnUnreadableFileOrUncreatableParentReturnsAnError", [] {
    LockDirectory t;
    const auto blocker = t.root / "file";
    { std::ofstream out(blocker); out << "not a directory"; }
    mcpp::pm::Lockfile lock;
    expect(!mcpp::pm::write(lock, blocker / "mcpp.lock"));
    expect(!mcpp::pm::write(lock, t.root));
}};


UTest updating_a_linked_lock_keeps_the_link_and_updates_its_target{"PmLockIo/UpdatingALinkedLockKeepsTheLinkAndUpdatesItsTarget", [] {
    LockDirectory t;
    const auto target = t.root / "target.lock";
    const auto link = t.root / "mcpp.lock";
    mcpp::pm::Lockfile lock;
    expect(fatal(bool(mcpp::pm::write(lock, target))));
    std::error_code ec;
    std::filesystem::create_symlink(target, link, ec);
    if (ec) skip([&] { std::ostringstream text; text << "this host does not permit file symlinks: " << ec.message(); return text.str(); }());
    lock.packages.push_back({"fixture", "example", "2.0.0", "path", "digest"});
    expect(fatal(bool(mcpp::pm::write(lock, link))));
    expect(std::filesystem::is_symlink(link));
    auto loaded = mcpp::pm::load(target);
    expect(fatal(bool(loaded)));
    expect(fatal(eq(loaded->packages.size(), 1u)));
    expect(eq(loaded->packages[0].version, "2.0.0"));
}};

// An entry names the configurations that resolve it, and reads back as written
// (SPEC-013 §15.1); a lock written before the field reads with none.
UTest targets_round_trip{"PmLockIo/TargetsRoundTrip", [] {
    const auto dir = std::filesystem::temp_directory_path() / std::format("mcpp-lock-{}",
        std::chrono::steady_clock::now().time_since_epoch().count());
    std::filesystem::create_directories(dir);
    mcpp::pm::Lockfile lock;
    lock.schemaVersion = 2;
    mcpp::pm::LockedPackage p;
    p.name = "openkal-musl";
    p.namespace_ = "mcpplibs";
    p.version = "0.22.6";
    p.source = "index+mcpplibs@0.22.6";
    p.hash = "fnv1a:0";
    p.origin = "platform";
    p.targets = { "aarch64-openkal", "x86_64-openkal" };
    lock.packages.push_back(p);
    expect(fatal(bool(mcpp::pm::write(lock, dir / "mcpp.lock"))));
    auto back = mcpp::pm::load(dir / "mcpp.lock");
    expect(fatal(bool(back)));
    expect(fatal(eq(back->packages.size(), 1u)));
    expect(eq(back->packages[0].targets, p.targets));
    expect(eq(back->packages[0].origin, "platform"));
    std::ofstream(dir / "old.lock") << "version = 2\n\n[package.\"x\"]\nnamespace = \"mcpplibs\"\nversion = \"1\"\n";
    auto old = mcpp::pm::load(dir / "old.lock");
    expect(fatal(bool(old)));
    expect(old->packages.at(0).targets.empty());
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}};
