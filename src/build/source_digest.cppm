// mcpp.build.source_digest — the content digest of a package's source tree,
// recomputed only where a file's size or modification time moved (mcpp#797).
//
// A build program depends on every source of the packages in its graph,
// headers a module unit includes among them, and for a `path` package those
// can change without its name or version changing. Its identity therefore
// holds the digest of each such tree (R6: hashing only the interface file left
// an edited header unseen, and the program was neither rebuilt nor rerun).
//
// Hashing every file on every build is what checking one host module entry
// used to cost (52 ms for a one-file package, measured). The digest is a
// function of the contents, and a file whose `(size, mtime)` is what the
// previous walk recorded is taken to have the contents recorded with it --
// ninja's own assumption. A file whose stat moved is read again, so a touched
// file whose bytes did not change leaves the digest as it was.
//
// WHAT IS WALKED. Every regular file below the root, except `target`, `.git`
// and `.mcpp`, and except a subdirectory holding its own `mcpp.toml`: that is
// another package (a workspace member), with its own digest where something
// depends on it -- the tool store's rule (SPEC-004 §10.1).

export module mcpp.build.source_digest;

import std;
import mcpp.toolchain.fingerprint;   // hash_file / hash_string

export namespace mcpp::build {

// The digest of the tree below `root`. `statCache` is where the previous walk's
// `(path, size, mtime, hash)` rows are kept; it is read when present and
// rewritten when anything moved. An unreadable cache is a cold walk.
std::string source_tree_digest(const std::filesystem::path& root,
                               const std::filesystem::path& statCache);

}  // namespace mcpp::build

namespace mcpp::build {

namespace fs = std::filesystem;

namespace {

struct Row {
    std::uintmax_t size = 0;
    long long      mtime = 0;
    std::string    hash;
};

std::string utf8(const fs::path& p) {
    const auto u = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

std::map<std::string, Row> read_cache(const fs::path& file) {
    std::map<std::string, Row> rows;
    std::ifstream in(file, std::ios::binary);
    std::string line;
    if (!std::getline(in, line) || line != "mcpp-source-digest-v1") return rows;
    while (std::getline(in, line)) {
        // path \t size \t mtime \t hash
        const auto a = line.find('\t');
        const auto b = a == std::string::npos ? a : line.find('\t', a + 1);
        const auto c = b == std::string::npos ? b : line.find('\t', b + 1);
        if (c == std::string::npos) continue;
        Row r;
        std::from_chars(line.data() + a + 1, line.data() + b, r.size);
        std::from_chars(line.data() + b + 1, line.data() + c, r.mtime);
        r.hash = line.substr(c + 1);
        rows.emplace(line.substr(0, a), std::move(r));
    }
    return rows;
}

}  // namespace

std::string source_tree_digest(const fs::path& root, const fs::path& statCache) {
    const auto previous = read_cache(statCache);
    std::map<std::string, Row> now;
    bool moved = false;
    std::error_code ec;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
    for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code tec;
        if (it->is_directory(tec)) {
            const auto name = utf8(it->path().filename());
            if (name == "target" || name == ".git" || name == ".mcpp"
                || fs::exists(it->path() / "mcpp.toml", tec))
                it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(tec)) continue;
        const auto rel = utf8(it->path().lexically_relative(root));
        Row r;
        r.size = it->file_size(tec);
        r.mtime = static_cast<long long>(
            it->last_write_time(tec).time_since_epoch().count());
        if (auto p = previous.find(rel);
            p != previous.end() && p->second.size == r.size && p->second.mtime == r.mtime) {
            r.hash = p->second.hash;
        } else {
            r.hash = mcpp::toolchain::hash_file(it->path());
            moved = true;
        }
        now.emplace(rel, std::move(r));
    }
    if (now.size() != previous.size()) moved = true;

    std::string text;
    for (auto const& [rel, r] : now) text += std::format("{}\t{}\n", rel, r.hash);
    const auto digest = mcpp::toolchain::hash_string(text);

    if (moved && !statCache.empty()) {
        fs::create_directories(statCache.parent_path(), ec);
        const auto tmp = statCache.string() + ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            out << "mcpp-source-digest-v1\n";
            for (auto const& [rel, r] : now)
                out << std::format("{}\t{}\t{}\t{}\n", rel, r.size, r.mtime, r.hash);
        }
        fs::rename(tmp, statCache, ec);
    }
    return digest;
}

}  // namespace mcpp::build
