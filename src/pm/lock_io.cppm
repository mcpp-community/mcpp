// mcpp.pm.lock_io — read & write mcpp.lock (TOML).
//
// Part of the package-management subsystem refactor; see
// `.agents/docs/2026-05-08-pm-subsystem-architecture.md`.
// Body unchanged from the previous `mcpp.lockfile` module — only the
// namespace + module name moved. The old `mcpp.lockfile` module is
// kept as a thin shim that re-exports the same names so existing
// callers compile unchanged. A later PR will migrate call sites to
// `mcpp::pm::` directly and the shim will be removed.
//
// v2 schema (0.0.14+): adds [indices.<name>] sections with url + rev,
// and `namespace` field to [package.*] entries. v1 files are migrated
// on load: all packages default to namespace="mcpplibs".

export module mcpp.pm.lock_io;

import std;
import mcpp.libs.toml;

export namespace mcpp::pm {

struct LockedIndex {
    std::string name;    // index name (key in [indices])
    std::string url;     // git URL
    std::string rev;     // locked commit sha (40 chars)
};

struct LockedPackage {
    std::string name;
    std::string namespace_;   // package namespace (v2+); empty = independent root
    std::string version;
    std::string source;     // e.g. "index+mcpplibs@abc123def..."
    std::string hash;       // "sha256:..." or "fnv1a:..."
};

// Parsed form of a git source string as written to mcpp.lock.
// Supported forms:
//   git+https://host/repo#branch=develop@5848943...
//   git+https://host/repo#tag=v1.0.0
//   git+https://host/repo#rev=5848943...
// Only `branch` carries `@<commit>`: a tag or rev already names a fixed point
// in history, whereas a branch is floating and the recorded commit is what
// pins it. `resolvedCommit` is therefore empty for tag/rev, and also for a
// branch entry written before the commit was known.
struct LockedGitSource {
    std::string url;
    std::string refKind;                       // "branch", "tag", or "rev"
    std::string ref;
    std::optional<std::string> resolvedCommit; // for branch entries with @commit
};

std::optional<LockedGitSource> parse_git_source(std::string_view source);

struct Lockfile {
    int                                 schemaVersion = 2;
    std::vector<LockedIndex>            indices;
    std::vector<LockedPackage>          packages;
};

struct LockError {
    std::string message;
};

std::expected<Lockfile, LockError> load(const std::filesystem::path& path);
std::expected<void, LockError>     write(const Lockfile& lock, const std::filesystem::path& path);

std::string serialize(const Lockfile& lock);
std::string compute_hash(const Lockfile& lock);

} // namespace mcpp::pm

namespace mcpp::pm {

namespace t = mcpp::libs::toml;

std::expected<Lockfile, LockError> load(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        return Lockfile{};      // no lock yet
    }
    auto doc = t::parse_file(path);
    if (!doc) return std::unexpected(LockError{
        std::format("{}:{}:{}: {}", path.string(), doc.error().where.line,
                    doc.error().where.column, doc.error().message)});

    Lockfile lock;
    int fileVersion = 1;
    if (auto v = doc->get_int("version"))      fileVersion = static_cast<int>(*v);

    // Always upgrade to v2 in memory.
    lock.schemaVersion = 2;

    // Parse [indices.<name>] sections (v2+).
    auto* idxTbl = doc->get_table("indices");
    if (idxTbl) {
        for (auto& [k, v] : *idxTbl) {
            if (!v.is_table()) continue;
            auto& tt = v.as_table();
            LockedIndex li;
            li.name = k;
            if (auto it = tt.find("url"); it != tt.end() && it->second.is_string()) li.url = it->second.as_string();
            if (auto it = tt.find("rev"); it != tt.end() && it->second.is_string()) li.rev = it->second.as_string();
            lock.indices.push_back(std::move(li));
        }
    }

    // [[package]] arrays are not in our minimal parser. We use [package.<name>] instead.
    // Or just iterate the root looking for top-level "package" table that contains a list.
    // For simplicity in M2: accept either format with top-level array of tables described
    // as [package.X] sections.
    auto* pkgs = doc->get_table("package");
    if (pkgs) {
        for (auto& [k, v] : *pkgs) {
            if (!v.is_table()) continue;
            auto& tt = v.as_table();
            LockedPackage lp;
            lp.name = k;
            if (auto it = tt.find("namespace"); it != tt.end() && it->second.is_string()) lp.namespace_ = it->second.as_string();
            if (auto it = tt.find("version");   it != tt.end() && it->second.is_string()) lp.version    = it->second.as_string();
            if (auto it = tt.find("source");    it != tt.end() && it->second.is_string()) lp.source     = it->second.as_string();
            if (auto it = tt.find("hash");      it != tt.end() && it->second.is_string()) lp.hash       = it->second.as_string();

            // v1 → v2 migration: default namespace to "mcpplibs".
            if (fileVersion < 2 && lp.namespace_.empty()) {
                lp.namespace_ = "mcpplibs";
            }

            lock.packages.push_back(std::move(lp));
        }
    }
    return lock;
}

std::string serialize(const Lockfile& lock) {
    std::string out;
    out += "# Auto-generated by mcpp. Do not edit by hand.\n";
    out += std::format("version = {}\n", lock.schemaVersion);

    // Write [indices.<name>] sections.
    for (auto& idx : lock.indices) {
        out += std::format("\n[indices.\"{}\"]\n", idx.name);
        out += std::format("url = {}\n", t::escape_string(idx.url));
        out += std::format("rev = {}\n", t::escape_string(idx.rev));
    }

    // Blank line before packages if we had indices or just after version.
    if (!lock.packages.empty()) out += "\n";

    for (auto& p : lock.packages) {
        out += std::format("[package.\"{}\"]\n", p.name);
        if (!p.namespace_.empty()) {
            out += std::format("namespace = {}\n", t::escape_string(p.namespace_));
        }
        out += std::format("version = {}\n", t::escape_string(p.version));
        out += std::format("source  = {}\n", t::escape_string(p.source));
        out += std::format("hash    = {}\n\n", t::escape_string(p.hash));
    }
    return out;
}

std::expected<void, LockError> write(const Lockfile& lock, const std::filesystem::path& path) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream os(path);
    if (!os) return std::unexpected(LockError{std::format("cannot write '{}'", path.string())});
    os << serialize(lock);
    return {};
}

std::string compute_hash(const Lockfile& lock) {
    // FNV-1a over the canonical serialized form.
    auto s = serialize(lock);
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 0x100000001b3ull;
    }
    return std::format("{:016x}", h);
}

std::optional<LockedGitSource> parse_git_source(std::string_view source) {
    constexpr std::string_view prefix = "git+";
    if (!source.starts_with(prefix)) return std::nullopt;

    auto rest = source.substr(prefix.size());
    auto hashPos = rest.find('#');
    if (hashPos == std::string_view::npos) return std::nullopt;

    LockedGitSource out;
    out.url = std::string(rest.substr(0, hashPos));
    auto fragment = rest.substr(hashPos + 1);

    // fragment is one of: branch=develop@commit, tag=v1.0.0, rev=abc123
    auto eqPos = fragment.find('=');
    if (eqPos == std::string_view::npos) return std::nullopt;

    out.refKind = std::string(fragment.substr(0, eqPos));
    if (out.refKind != "branch" && out.refKind != "tag" && out.refKind != "rev")
        return std::nullopt;

    auto refPart = fragment.substr(eqPos + 1);
    out.ref = std::string(refPart);
    // Only branch entries carry `@<commit>` — see the writer in prepare.cppm,
    // which appends it for `branch` and nothing else. Splitting unconditionally
    // would read a tag legitimately named `v1@rc` as ref `v1` plus commit `rc`.
    // Within branch entries, split on the LAST `@` so a branch name that
    // contains one ("feat@v2") still round-trips.
    if (out.refKind == "branch") {
        if (auto atPos = refPart.rfind('@'); atPos != std::string_view::npos) {
            out.ref = std::string(refPart.substr(0, atPos));
            // A bare `branch=foo@` records no commit; leaving an empty string
            // behind would make the anchor claim a pin it does not have.
            if (auto commit = refPart.substr(atPos + 1); !commit.empty())
                out.resolvedCommit = std::string(commit);
        }
    }

    if (out.ref.empty()) return std::nullopt;
    return out;
}

} // namespace mcpp::pm
