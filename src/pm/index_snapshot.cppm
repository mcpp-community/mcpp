// mcpp.pm.index_snapshot — local, known-good copies of index trees, and the
// guard that makes a refresh MONOTONE.
//
// THE INVARIANT
//
//   An index-side change must never take mcpp from "works" to "does not work".
//
// It did not hold. An index tree can declare a client-version floor
// (`index.toml` `min_mcpp`, mcpp.pm.index_contract). When the published index
// raises that floor, `xlings update` replaces the local tree in place with one
// this binary cannot read, every descriptor read starts returning nothing, and
// the build dies — on a machine that was working sixty seconds earlier. There
// was no backup, so there was no way back either: the refresh is what broke it.
//
// The 2026-07-08 index design specified the cure ("staged refresh keeps the
// last compatible snapshot") and it was never implemented — the floor was
// checked in exactly one place, the descriptor reader, and the refresh path
// knew nothing about it.
//
// WHY BACKUP-AND-ROLLBACK RATHER THAN STAGE-AND-SWAP
//
// mcpp does not fetch indexes. `mcpp::xlings::update_index` shells out to
// `xlings update`, which rewrites the tree in place; mcpp cannot ask it to
// unpack somewhere else (the `index_repos` entry it seeds carries name / url /
// artifact / source and no version or destination). So the sequence is
// archive → let the refresh happen → judge the result → restore if it got
// worse. Same guarantee, no cross-repo dependency.
//
// The judgement is deliberately "did it get WORSE", not "is it good":
//
//     usable before && !usable after   → restore
//
// A tree that was already unusable has nothing to roll back to, and restoring
// an equally-unusable snapshot over a fresh one would only make the next
// refresh redo the same work.
//
// WHY KEEP MORE THAN ONE
//
// One backup answers "undo the refresh that just happened". A short history
// answers "give me the newest snapshot this binary can actually read", which
// is the same question the publishing side would answer with a version
// negotiation protocol (openxlings/xlings#476) — locally, for free, and today.
// It only covers snapshots this machine has seen, which is the overwhelmingly
// common case for a development box or a CI runner with a warm cache; a fresh
// install that meets a too-new index has no local history and correctly falls
// through to "upgrade mcpp".
//
// COST
//
// Index trees are small (measured: 912 KB for mcpplibs, 2.2 MB for
// xim-pkgindex) and archiving uses hard links where the filesystem allows it,
// so a snapshot costs approximately one directory walk and no extra bytes.

export module mcpp.pm.index_snapshot;

import std;
import mcpp.pm.index_contract;

export namespace mcpp::pm::index_snapshot {

// How many known-good snapshots to keep per index tree. Small on purpose: the
// value of the history is "the last few floors", and floors move rarely (the
// mcpp index has changed its floor 6 times in its entire life).
inline constexpr std::size_t kKeepPerIndex = 5;

// The snapshot store is a SIBLING of the index data root, never inside it:
//
//     <dataRoot>/..                 index-snapshots/<index-dir-name>/<snapshot-id>/
//
// Everything that enumerates index repos does it by listing directories under
// `data/` (mcpp's own Fetcher::sorted_index_dirs takes every subdirectory, and
// xlings walks the same tree). A snapshot store living there would be handed to
// those scanners as if it were an index. Today that happens to be harmless —
// the store has no `pkgs/` so lookups miss and move on — but "harmless because
// of a detail of someone else's loop" is not a property worth depending on,
// and it costs nothing to put the store where no index scanner can reach it.
std::filesystem::path snapshots_root(const std::filesystem::path& dataRoot);
std::filesystem::path snapshot_dir(const std::filesystem::path& dataRoot,
                                   const std::filesystem::path& indexDir);

// Identity of the tree currently in `indexDir`, taken from the marker xlings
// writes (`.xlings-index-version`). OPAQUE BY CONTRACT — a short sha for the
// artifact transport, a version string for others; never parsed, only compared
// and used as a directory name. Empty when the marker is absent, in which case
// the caller falls back to a content-independent name.
std::string snapshot_id(const std::filesystem::path& indexDir);

// Archive `indexDir` under the snapshots root. No-op (returns false) when the
// tree is unusable — a snapshot exists to be restored, and restoring an
// unusable tree helps nobody. Overwrites an existing snapshot with the same id.
bool archive(const std::filesystem::path& dataRoot,
             const std::filesystem::path& indexDir);

// Restore a specific snapshot over `indexDir`. The destination is replaced.
bool restore(const std::filesystem::path& snapshotPath,
             const std::filesystem::path& indexDir);

// Newest-first list of archived snapshots for one index tree.
std::vector<std::filesystem::path>
list_snapshots(const std::filesystem::path& dataRoot,
               const std::filesystem::path& indexDir);

// The newest archived snapshot this binary can actually read, if any.
std::optional<std::filesystem::path>
newest_usable(const std::filesystem::path& dataRoot,
              const std::filesystem::path& indexDir);

// Drop all but the newest `keep` snapshots of one index tree.
void prune(const std::filesystem::path& dataRoot,
           const std::filesystem::path& indexDir,
           std::size_t keep = kKeepPerIndex);

// Index trees directly under `dataRoot`: a directory containing `pkgs/`.
std::vector<std::filesystem::path>
index_dirs(const std::filesystem::path& dataRoot);

// What a guarded refresh did, for the caller to report.
struct GuardOutcome {
    // Index trees that were usable before the refresh and unusable after, and
    // were therefore rolled back.
    std::vector<std::filesystem::path> rolledBack;
    // Index trees restored from an older snapshot because the refreshed tree
    // was unusable and no pre-refresh tree existed to keep.
    std::vector<std::filesystem::path> recovered;
    // Index trees left unusable — nothing local could serve them.
    std::vector<std::filesystem::path> stillUnusable;

    bool degraded() const {
        return !rolledBack.empty() || !recovered.empty() || !stillUnusable.empty();
    }
};

// Run `refresh` with the monotonicity guarantee over every index tree under
// `dataRoot`. Returns the refresh's own exit code; `out` describes what the
// guard had to do.
//
// `refresh` is a callable so this module stays free of any dependency on the
// xlings process layer — and so it is testable without a network.
int guarded_refresh(const std::filesystem::path& dataRoot,
                    const std::function<int()>& refresh,
                    GuardOutcome& out);

} // namespace mcpp::pm::index_snapshot

namespace mcpp::pm::index_snapshot {

namespace {

// A REAL copy. Not hard links.
//
// Hard links were the obvious optimisation — an index tree is thousands of
// small files and a snapshot that costs real bytes is a reason not to take one
// — and they are wrong here, which the round-trip test caught immediately: a
// hard-linked snapshot ALIASES the live tree. Anything that rewrites an index
// file in place (a truncating write, a tar extraction over an existing path)
// writes straight through the link and destroys the backup. The one event this
// module exists to survive is precisely "something replaced the tree", so a
// snapshot that shares inodes with the tree is not a snapshot at all.
//
// The bytes are cheap in absolute terms (measured: 912 KB for mcpplibs, 2.2 MB
// for xim-pkgindex; five kept snapshots ≈ 15 MB) and `prune` bounds the total.
bool copy_tree(const std::filesystem::path& from,
               const std::filesystem::path& to)
{
    std::error_code ec;
    std::filesystem::remove_all(to, ec);
    ec.clear();
    std::filesystem::create_directories(to.parent_path(), ec);

    ec.clear();
    std::filesystem::copy(from, to,
        std::filesystem::copy_options::recursive
            | std::filesystem::copy_options::overwrite_existing
            | std::filesystem::copy_options::skip_symlinks, ec);
    return !ec;
}

// Directory mtime, as a sortable integer. Only ever compared against other
// snapshots of the same index, so file_clock's non-Unix epoch is irrelevant.
std::int64_t mtime_of(const std::filesystem::path& p) {
    std::error_code ec;
    auto t = std::filesystem::last_write_time(p, ec);
    if (ec) return 0;
    return t.time_since_epoch().count();
}

std::string sanitize_component(std::string s) {
    for (auto& c : s) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                     || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
        if (!ok) c = '_';
    }
    if (s.empty()) s = "unknown";
    return s;
}

} // namespace

std::filesystem::path snapshots_root(const std::filesystem::path& dataRoot) {
    auto parent = dataRoot.parent_path();
    // Degenerate path (relative "data", or a root) — fall back to staying put
    // rather than climbing out of the home directory.
    if (parent.empty()) return dataRoot / ".index-snapshots";
    return parent / "index-snapshots";
}

std::filesystem::path snapshot_dir(const std::filesystem::path& dataRoot,
                                   const std::filesystem::path& indexDir) {
    return snapshots_root(dataRoot) / indexDir.filename();
}

std::string snapshot_id(const std::filesystem::path& indexDir) {
    std::ifstream is(indexDir / ".xlings-index-version", std::ios::binary);
    if (!is) return {};
    std::string v{std::istreambuf_iterator<char>(is), {}};
    while (!v.empty() && (v.back() == '\n' || v.back() == '\r' || v.back() == ' '))
        v.pop_back();
    return sanitize_component(std::move(v));
}

std::vector<std::filesystem::path>
index_dirs(const std::filesystem::path& dataRoot) {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    if (!std::filesystem::is_directory(dataRoot, ec)) return out;
    for (auto& e : std::filesystem::directory_iterator(dataRoot, ec)) {
        if (ec) break;
        if (!e.is_directory()) continue;
        // The store is a sibling of dataRoot (see snapshots_root), so it
        // should never appear here. Skipped anyway: this loop decides what
        // gets archived, and an archive of the archive is the one mistake
        // that would grow without bound.
        if (e.path().filename() == ".index-snapshots") continue;
        std::error_code pec;
        if (!std::filesystem::is_directory(e.path() / "pkgs", pec)) continue;
        out.push_back(e.path());
    }
    std::ranges::sort(out);
    return out;
}

bool archive(const std::filesystem::path& dataRoot,
             const std::filesystem::path& indexDir)
{
    if (!mcpp::pm::index_usable(indexDir)) return false;

    auto id = snapshot_id(indexDir);
    if (id.empty()) id = "unversioned";
    auto dst = snapshot_dir(dataRoot, indexDir) / id;

    // Same id already archived: the tree did not move, so the existing
    // snapshot is byte-equivalent and re-copying it buys nothing.
    std::error_code ec;
    if (std::filesystem::is_directory(dst, ec) && id != "unversioned")
        return true;

    return copy_tree(indexDir, dst);
}

bool restore(const std::filesystem::path& snapshotPath,
             const std::filesystem::path& indexDir)
{
    std::error_code ec;
    if (!std::filesystem::is_directory(snapshotPath, ec)) return false;
    return copy_tree(snapshotPath, indexDir);
}

std::vector<std::filesystem::path>
list_snapshots(const std::filesystem::path& dataRoot,
               const std::filesystem::path& indexDir)
{
    std::vector<std::filesystem::path> out;
    auto base = snapshot_dir(dataRoot, indexDir);
    std::error_code ec;
    if (!std::filesystem::is_directory(base, ec)) return out;
    for (auto& e : std::filesystem::directory_iterator(base, ec)) {
        if (ec) break;
        if (e.is_directory()) out.push_back(e.path());
    }
    std::ranges::sort(out, [](auto& a, auto& b) {
        return mtime_of(a) > mtime_of(b);       // newest first
    });
    return out;
}

std::optional<std::filesystem::path>
newest_usable(const std::filesystem::path& dataRoot,
              const std::filesystem::path& indexDir)
{
    for (auto& s : list_snapshots(dataRoot, indexDir))
        if (mcpp::pm::index_usable(s)) return s;
    return std::nullopt;
}

void prune(const std::filesystem::path& dataRoot,
           const std::filesystem::path& indexDir,
           std::size_t keep)
{
    auto snaps = list_snapshots(dataRoot, indexDir);
    for (std::size_t i = keep; i < snaps.size(); ++i) {
        std::error_code ec;
        std::filesystem::remove_all(snaps[i], ec);
    }
}

int guarded_refresh(const std::filesystem::path& dataRoot,
                    const std::function<int()>& refresh,
                    GuardOutcome& out)
{
    // Snapshot every tree that is currently readable. Doing this BEFORE the
    // refresh is the whole point: afterwards the old bytes are gone.
    auto before = index_dirs(dataRoot);
    std::map<std::filesystem::path, bool> usableBefore;
    for (auto& dir : before) {
        usableBefore[dir] = mcpp::pm::index_usable(dir);
        if (usableBefore[dir]) {
            archive(dataRoot, dir);
            prune(dataRoot, dir);
        }
    }

    const int rc = refresh();

    // Judge each tree. `index_dirs` is re-read: a refresh may add a repo.
    for (auto& dir : index_dirs(dataRoot)) {
        if (mcpp::pm::index_usable(dir)) {
            // Improved or unchanged — record the new good state for next time.
            archive(dataRoot, dir);
            prune(dataRoot, dir);
            continue;
        }
        auto wasUsable = usableBefore.find(dir);
        if (wasUsable != usableBefore.end() && wasUsable->second) {
            // Got worse. This is the case the invariant exists for.
            if (auto snap = newest_usable(dataRoot, dir);
                snap && restore(*snap, dir)) {
                out.rolledBack.push_back(dir);
                continue;
            }
        }
        // Was already unusable (or the rollback failed): try the local history
        // anyway — it may hold a readable tree from before this binary ever
        // met the raised floor.
        if (auto snap = newest_usable(dataRoot, dir);
            snap && restore(*snap, dir)) {
            out.recovered.push_back(dir);
            continue;
        }
        out.stillUnusable.push_back(dir);
    }
    return rc;
}

} // namespace mcpp::pm::index_snapshot
