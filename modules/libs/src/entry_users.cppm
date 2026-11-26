// mcpp.libs.entry_users — the source trees that use a stored entry.
//
// An entry of the build cache (a `path` dependency's objects) or of the tool
// store (a `path` host tool) is addressed by the content of the tree it was
// built from. Two checkouts of one commit have the same content and share the
// entry. An edit of one checkout gives that checkout a new entry, and its
// previous one is no longer needed by it -- but it may be by the other.
//
// The record was the one tree that wrote the entry (`source`), and the entry
// was removed when that tree was built under another digest: under a build of
// the other checkout that named the entry's files, which then failed at the
// next edge that read them (mcpp#1025). The two stores held the same rule in
// two places, each a copy of the other.
//
// THE RECORD IS A SET, ONE FILE PER TREE. `users/<digest of the tree>` holds the
// tree's path. A tree that hits or writes the entry adds its file; a tree built
// under another digest removes its file from the entries it used before; an
// entry whose `users/` is then empty is removed. Each step is the creation or
// removal of one file, so two processes acting at once lose nothing: there is
// no file that both read and rewrite.
//
// THE FORMER RECORD IS READ, NEVER WRITTEN. An entry written before the set
// holds `source`, read as one user. The set is a directory of another name
// because releases before it share homes with releases after it (a project's
// bootstrap mcpp and the one it builds): an older release finds no `source` in
// an entry written now and leaves the entry alone, where a `source` of several
// lines would have been read as one path that matches no tree.
//
// A checkout that is deleted never removes its file; its entries are
// collected by the last-use rule of the stores (`[cache] unused_days`).
//
// It imports nothing of mcpp.

export module mcpp.libs.entry_users;

import std;

export namespace mcpp::libs::entry_users {

// The trees `entry` records, in no particular order: its `users/`, else its
// former `source`.
std::vector<std::string> users(const std::filesystem::path& entry);

// `tree` recorded as a user of `entry`; nothing is written when it is one.
void add(const std::filesystem::path& entry, const std::filesystem::path& tree);

// `entry` was written for `tree`, which no longer uses the entries it was built
// into under another digest: `tree` is added to `entry` and taken from every
// sibling, and a sibling left with no user is removed.
//
// The siblings are the entries `<store>/<package>@<version>+path.<digest>/<key>/`
// of the same package and version whose digest differs, the layout both stores
// share: `entry`'s grandparent holds them, and `versionPrefix` is
// `<package>@<version>+path.`.
void supersede(const std::filesystem::path& entry, std::string_view versionPrefix,
               const std::filesystem::path& tree);

} // namespace mcpp::libs::entry_users

namespace mcpp::libs::entry_users {

namespace {

namespace fs = std::filesystem;

std::string spelling(const fs::path& tree) {
    const auto u8 = tree.lexically_normal().generic_u8string();
    return {u8.begin(), u8.end()};
}

// FNV-1a over the tree's spelling: a file name that every file system takes,
// stable across releases and compilers (std::hash is neither).
std::string file_of(std::string_view tree) {
    std::uint64_t h = 1469598103934665603ull;
    for (unsigned char c : tree) {
        h ^= c;
        h *= 1099511628211ull;
    }
    return std::format("{:016x}", h);
}

std::optional<std::string> read_text(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return std::nullopt;
    return std::string{std::istreambuf_iterator<char>(in), {}};
}

// `tree` taken from `entry`; the entry removed when no user is left.
void take(const fs::path& entry, std::string_view tree) {
    std::error_code ec;
    const auto dir = entry / "users";
    if (fs::is_directory(dir, ec)) {
        fs::remove(dir / file_of(tree), ec);
        if (fs::is_empty(dir, ec) && !ec) fs::remove_all(entry, ec);
        return;
    }
    // The former record: one tree. Removed as it was removed before the set.
    if (auto was = read_text(entry / "source"); was && *was == tree) fs::remove_all(entry, ec);
}

} // namespace

std::vector<std::string> users(const fs::path& entry) {
    std::vector<std::string> out;
    std::error_code ec;
    if (fs::is_directory(entry / "users", ec)) {
        for (auto const& f : fs::directory_iterator(entry / "users", ec))
            if (auto text = read_text(f.path())) out.push_back(std::move(*text));
        return out;
    }
    if (auto was = read_text(entry / "source")) out.push_back(std::move(*was));
    return out;
}

void add(const fs::path& entry, const fs::path& tree) {
    const auto text = spelling(tree);
    std::error_code ec;
    const auto dir = entry / "users";
    const auto file = dir / file_of(text);
    if (fs::exists(file, ec)) return;
    // An entry with the former record keeps its tree as a user of the set.
    if (!fs::is_directory(dir, ec))
        if (auto was = read_text(entry / "source"); was && *was != text) {
            fs::create_directories(dir, ec);
            std::ofstream(dir / file_of(*was), std::ios::binary) << *was;
        }
    fs::create_directories(dir, ec);
    // Written under a name of its own and renamed: a reader never sees a part.
    auto tmp = dir / std::format(".{}.{:x}.tmp", file_of(text), std::random_device{}());
    {
        std::ofstream os(tmp, std::ios::binary);
        os << text;
        if (!os.flush()) {
            os.close();
            fs::remove(tmp, ec);
            return;
        }
    }
    fs::rename(tmp, file, ec);
    if (ec) fs::remove(tmp, ec);
}

void supersede(const fs::path& entry, std::string_view versionPrefix, const fs::path& tree) {
    const auto text = spelling(tree);
    add(entry, tree);
    const auto versionDir = entry.parent_path();
    std::error_code ec;
    for (auto const& v : fs::directory_iterator(versionDir.parent_path(), ec)) {
        const auto name = v.path().filename().u8string();
        if (v.path() == versionDir || !std::string(name.begin(), name.end()).starts_with(versionPrefix))
            continue;
        std::error_code kec;
        std::vector<fs::path> keys;
        for (auto const& key : fs::directory_iterator(v.path(), kec)) keys.push_back(key.path());
        for (auto const& key : keys) take(key, text);
        fs::remove(v.path(), kec);   // only when nothing else is left in it
    }
}

} // namespace mcpp::libs::entry_users
