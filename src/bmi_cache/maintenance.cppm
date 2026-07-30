// mcpp.bmi_cache.maintenance — inspection and pruning of the global build
// cache, plus the shared fs-size/byte-formatting helpers they are built on.
//
// Layout walked here (see bmi_cache.cppm):
//   $MCPP_HOME/build-cache/v1/pkg/<index>/<pkg>@<ver>/<key16>/entry.json
//   $MCPP_HOME/build-cache/v1/std/<identity>/std-module.json
//
// Two things this file did wrong before the layout change and that the new
// entry.json fixes:
//
//   * pruning ranked entries by the DIRECTORY's mtime, which only ever recorded
//     when an entry was written. A dependency that hit on every single build
//     looked as stale as one nobody had touched in a month, so `prune` was
//     "drop what was populated long ago", not an LRU. entry.json carries an
//     `accessed` stamp that bmi_cache::touch_accessed refreshes on every hit.
//   * `clean` began with remove_all(<root>/"deps"), a path that never existed
//     (dep entries lived at <root>/<fp>/deps) — dead code in front of the loop
//     that actually did the work.

module;
#include <cstdio>
#include <cstdlib>

export module mcpp.bmi_cache.maintenance;

import std;
import mcpp.home;
import mcpp.libs.json;
import mcpp.ui;

namespace mcpp::bmi_cache {


export std::uintmax_t dir_size(const std::filesystem::path& p) {
    std::error_code ec;
    if (!std::filesystem::exists(p, ec)) return 0;
    std::uintmax_t total = 0;
    for (auto& e : std::filesystem::recursive_directory_iterator(p, ec)) {
        if (ec) break;
        std::error_code ec2;
        if (e.is_regular_file(ec2) && !ec2) {
            total += e.file_size(ec2);
        }
    }
    return total;
}

export std::string human_bytes(std::uintmax_t n) {
    constexpr const char* units[] = {"B", "KiB", "MiB", "GiB", "TiB"};
    double v = static_cast<double>(n);
    int u = 0;
    while (v >= 1024.0 && u < 4) { v /= 1024.0; ++u; }
    return std::format("{:.1f} {}", v, units[u]);
}

// Parse `<N>{s,m,h,d}` into seconds.
export std::optional<std::int64_t> parse_duration(std::string_view v) {
    if (v.size() < 2) return std::nullopt;
    char unit = v.back();
    std::int64_t n = 0;
    try { n = std::stoll(std::string(v.substr(0, v.size() - 1))); }
    catch (...) { return std::nullopt; }
    switch (unit) {
        case 's': return n;
        case 'm': return n * 60;
        case 'h': return n * 3600;
        case 'd': return n * 86400;
        default:  return std::nullopt;
    }
}

// Parse `<N>{B,KiB,MiB,GiB,TiB}` (suffix optional, case-insensitive, bare
// number = bytes) into bytes.
export std::optional<std::uintmax_t> parse_size(std::string_view v) {
    std::size_t digits = 0;
    while (digits < v.size() && std::isdigit(static_cast<unsigned char>(v[digits])))
        ++digits;
    if (digits == 0) return std::nullopt;
    std::uintmax_t n = 0;
    try { n = std::stoull(std::string(v.substr(0, digits))); }
    catch (...) { return std::nullopt; }
    std::string suffix;
    for (auto c : v.substr(digits))
        if (!std::isspace(static_cast<unsigned char>(c)))
            suffix.push_back(static_cast<char>(std::tolower(
                static_cast<unsigned char>(c))));
    if (suffix.empty() || suffix == "b")   return n;
    if (suffix == "k" || suffix == "kib" || suffix == "kb") return n * 1024ull;
    if (suffix == "m" || suffix == "mib" || suffix == "mb") return n * 1024ull * 1024;
    if (suffix == "g" || suffix == "gib" || suffix == "gb") return n * 1024ull * 1024 * 1024;
    if (suffix == "t" || suffix == "tib" || suffix == "tb")
        return n * 1024ull * 1024 * 1024 * 1024;
    return std::nullopt;
}

namespace {

std::filesystem::path pkg_root()    { return mcpp::home::cache_root() / "pkg"; }
std::filesystem::path std_root()    { return mcpp::home::cache_root() / "std"; }

struct Entry {
    std::filesystem::path dir;
    std::string           kind;        // "pkg" | "std"
    std::string           label;       // "<index>/<pkg>@<ver>" | "std"
    std::string           key;
    std::uintmax_t        size = 0;
    std::int64_t          accessed = 0; // seconds since epoch; 0 = unknown
    std::size_t           fileCount = 0;
    bool                  complete = true;
    std::string           problem;
};

std::optional<nlohmann::json> read_json(const std::filesystem::path& p) {
    std::ifstream is(p);
    if (!is) return std::nullopt;
    nlohmann::json j;
    try { is >> j; } catch (...) { return std::nullopt; }
    return j;
}

std::int64_t stamp_of(const nlohmann::json& j, const char* field) {
    auto it = j.find(field);
    if (it == j.end()) return 0;
    if (it->is_number_integer()) return it->get<std::int64_t>();
    if (it->is_string()) {
        try { return std::stoll(it->get<std::string>()); } catch (...) {}
    }
    return 0;
}

// mtime fallback for entries with no `accessed` stamp (std entries, and package
// entries written by an mcpp that predates the field).
//
// file_time_type is std::chrono::file_clock, whose epoch is NOT the Unix epoch —
// reading time_since_epoch() and comparing it against system_clock produced ages
// like "74509d ago". `clock_cast` would be the standard answer but libc++ does
// not provide it, so measure the mtime as an OFFSET FROM NOW in the file clock
// and apply that offset to the system clock. That needs no shared epoch and no
// conversion trait — and "how long ago" is all any caller wants anyway.
std::int64_t dir_mtime_seconds(const std::filesystem::path& p) {
    std::error_code ec;
    auto t = std::filesystem::last_write_time(p, ec);
    if (ec) return 0;
    auto age = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::file_clock::now() - t);
    auto nowSys = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch());
    return (nowSys - age).count();
}

void measure(Entry& e) {
    e.size = dir_size(e.dir);
    std::error_code ec;
    for (auto& _ : std::filesystem::recursive_directory_iterator(e.dir, ec)) {
        (void)_;
        if (ec) break;
        ++e.fileCount;
    }
}

// Verify a package entry's file list against the disk.
void check_pkg_files(Entry& e, const nlohmann::json& j) {
    std::error_code ec;
    auto missing = [&](const std::filesystem::path& p) {
        return !std::filesystem::exists(p, ec);
    };
    if (auto it = j.find("bmi"); it != j.end() && it->is_array()) {
        for (auto& v : *it) {
            if (!v.is_string()) continue;
            if (missing(e.dir / "bmi" / v.get<std::string>())) {
                e.complete = false;
                e.problem = std::format("missing bmi/{}", v.get<std::string>());
                return;
            }
        }
    }
    if (auto it = j.find("obj"); it != j.end() && it->is_array()) {
        for (auto& v : *it) {
            if (!v.is_string()) continue;
            if (missing(e.dir / "obj" / v.get<std::string>())) {
                e.complete = false;
                e.problem = std::format("missing obj/{}", v.get<std::string>());
                return;
            }
        }
    }
}

std::vector<Entry> walk_pkg_entries(bool verify = false) {
    std::vector<Entry> out;
    std::error_code ec;
    auto root = pkg_root();
    if (!std::filesystem::exists(root, ec)) return out;
    for (auto& idx : std::filesystem::directory_iterator(root, ec)) {
        if (!idx.is_directory()) continue;
        for (auto& pkg : std::filesystem::directory_iterator(idx.path(), ec)) {
            if (!pkg.is_directory()) continue;
            for (auto& key : std::filesystem::directory_iterator(pkg.path(), ec)) {
                if (!key.is_directory()) continue;
                Entry e;
                e.dir   = key.path();
                e.kind  = "pkg";
                e.key   = key.path().filename().string();
                e.label = idx.path().filename().string() + "/"
                        + pkg.path().filename().string();
                auto j = read_json(e.dir / "entry.json");
                if (!j) {
                    e.complete = false;
                    e.problem  = "no readable entry.json";
                } else {
                    e.accessed = stamp_of(*j, "accessed");
                    if (verify) check_pkg_files(e, *j);
                }
                if (e.accessed == 0) e.accessed = dir_mtime_seconds(e.dir);
                measure(e);
                out.push_back(std::move(e));
            }
        }
    }
    return out;
}

std::vector<Entry> walk_std_entries() {
    std::vector<Entry> out;
    std::error_code ec;
    auto root = std_root();
    if (!std::filesystem::exists(root, ec)) return out;
    for (auto& d : std::filesystem::directory_iterator(root, ec)) {
        if (!d.is_directory()) continue;
        Entry e;
        e.dir  = d.path();
        e.kind = "std";
        e.key  = d.path().filename().string();
        auto j = read_json(e.dir / "std-module.json");
        if (!j) {
            e.complete = false;
            e.problem  = "no readable std-module.json";
            e.label    = "std";
        } else {
            e.label = std::format("std {}@{} {} {}",
                                  j->value("compiler", std::string{"?"}),
                                  j->value("compiler_version", std::string{"?"}),
                                  j->value("cpp_standard", std::string{"?"}),
                                  j->value("stdlib", std::string{"?"}));
        }
        e.accessed = dir_mtime_seconds(e.dir);
        measure(e);
        out.push_back(std::move(e));
    }
    return out;
}

std::string format_age(std::int64_t stampSeconds) {
    if (stampSeconds == 0) return "unknown";
    auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    auto diff = now - stampSeconds;
    if (diff < 0)     return "just now";
    if (diff < 60)    return std::format("{}s ago", diff);
    if (diff < 3600)  return std::format("{}m ago", diff / 60);
    if (diff < 86400) return std::format("{}h ago", diff / 3600);
    return std::format("{}d ago", diff / 86400);
}

std::uintmax_t remove_entry(const Entry& e) {
    std::error_code ec;
    std::filesystem::remove_all(e.dir, ec);
    if (ec) return 0;
    // Drop the now-empty <pkg>@<ver> and <index> directories so `cache list`
    // does not accumulate empty shells.
    for (auto p = e.dir.parent_path();
         p != mcpp::home::cache_root() && p.has_parent_path();
         p = p.parent_path()) {
        std::error_code rmEc;
        if (!std::filesystem::remove(p, rmEc)) break;   // non-empty: stop
    }
    return e.size;
}

} // namespace

// `mcpp cache dir` — where IS the cache? Before this, `cache *`, `doctor` and
// `clean --bmi-cache` each resolved the root themselves while config.cppm's
// reset path used GlobalConfig::bmiCacheDir, and the copies could disagree.
export int cache_dir() {
    std::println("{}", mcpp::home::cache_root().string());
    auto legacy = mcpp::home::legacy_bmi_root();
    std::error_code ec;
    if (std::filesystem::exists(legacy, ec)) {
        std::println("legacy (unused, removable with `mcpp cache clean --legacy`): {}",
                     legacy.string());
    }
    return 0;
}

// `mcpp cache list [--json]`.
export int cache_list(bool asJson) {
    auto entries = walk_pkg_entries();
    auto stds    = walk_std_entries();

    if (asJson) {
        nlohmann::json j;
        j["root"] = mcpp::home::cache_root().string();
        j["entries"] = nlohmann::json::array();
        for (auto* set : {&entries, &stds}) {
            for (auto& e : *set) {
                j["entries"].push_back({
                    {"kind", e.kind},
                    {"label", e.label},
                    {"key", e.key},
                    {"dir", e.dir.string()},
                    {"bytes", e.size},
                    {"files", e.fileCount},
                    {"accessed", e.accessed},
                    {"complete", e.complete},
                });
            }
        }
        std::println("{}", j.dump(2));
        return 0;
    }

    if (entries.empty() && stds.empty()) {
        std::println("(build cache is empty)");
        return 0;
    }
    std::println("{:<16}  {:<6}  {:>10}  {:>14}  {}",
                 "key", "kind", "size", "last used", "package");
    std::uintmax_t total = 0;
    for (auto* set : {&stds, &entries}) {
        for (auto& e : *set) {
            total += e.size;
            std::println("{:<16}  {:<6}  {:>10}  {:>14}  {}{}",
                         e.key.substr(0, 16), e.kind, human_bytes(e.size),
                         format_age(e.accessed), e.label,
                         e.complete ? "" : "  (incomplete)");
        }
    }
    std::println("");
    std::println("{} entries, {}", entries.size() + stds.size(), human_bytes(total));
    return 0;
}

// `mcpp cache info <pkg>@<ver>`.
export int cache_info(const std::string& needle) {
    auto entries = walk_pkg_entries(/*verify=*/true);
    bool found = false;
    for (auto& e : entries) {
        if (e.label.find(needle) == std::string::npos) continue;
        found = true;
        std::println("dir          = {}", e.dir.string());
        std::println("key          = {}", e.key);
        std::println("package      = {}", e.label);
        std::println("size         = {}", human_bytes(e.size));
        std::println("file count   = {}", e.fileCount);
        std::println("last used    = {}", format_age(e.accessed));
        std::println("complete     = {}{}", e.complete ? "yes" : "no",
                     e.complete ? "" : "  (" + e.problem + ")");
        if (auto j = read_json(e.dir / "entry.json"); j && j->contains("inputs")) {
            std::println("inputs       =");
            std::println("{}", (*j)["inputs"].dump(2));
        }
        std::println("");
    }
    if (!found) {
        std::println("no cache entry matching '{}'", needle);
        return 1;
    }
    return 0;
}

// `mcpp cache prune --older-than <N>{s,m,h,d}` — kept as the age-only form.
export int cache_prune(const std::string& v) {
    if (v.empty()) {
        mcpp::ui::error("`mcpp cache prune` requires --older-than <N>{s,m,h,d}");
        return 2;
    }
    auto secs = parse_duration(v);
    if (!secs) {
        mcpp::ui::error(std::format(
            "bad --older-than value '{}' (expected <N>{{s,m,h,d}})", v));
        return 2;
    }
    auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    auto cutoff = now - *secs;

    int removed = 0;
    std::uintmax_t freed = 0;
    for (auto& e : walk_pkg_entries()) {
        if (e.accessed != 0 && e.accessed >= cutoff) continue;
        auto n = remove_entry(e);
        if (n || !std::filesystem::exists(e.dir)) {
            ++removed;
            freed += e.size;
            mcpp::ui::status("Pruned",
                std::format("{} ({})", e.label, human_bytes(e.size)));
        }
    }
    std::println("");
    std::println("Pruned {} entries, freed {}", removed, human_bytes(freed));
    return 0;
}

// `mcpp cache gc [--max-size N] [--older-than N]` — real LRU, driven by
// entry.json's `accessed` stamp.
export int cache_gc(const std::string& maxSizeArg, const std::string& olderThanArg) {
    if (maxSizeArg.empty() && olderThanArg.empty()) {
        mcpp::ui::error("`mcpp cache gc` requires --max-size <N>{MiB,GiB} "
                        "and/or --older-than <N>{s,m,h,d}");
        return 2;
    }
    std::optional<std::uintmax_t> maxSize;
    if (!maxSizeArg.empty()) {
        maxSize = parse_size(maxSizeArg);
        if (!maxSize) {
            mcpp::ui::error(std::format(
                "bad --max-size value '{}' (expected <N>{{B,KiB,MiB,GiB,TiB}})",
                maxSizeArg));
            return 2;
        }
    }
    std::optional<std::int64_t> olderThan;
    if (!olderThanArg.empty()) {
        olderThan = parse_duration(olderThanArg);
        if (!olderThan) {
            mcpp::ui::error(std::format(
                "bad --older-than value '{}' (expected <N>{{s,m,h,d}})", olderThanArg));
            return 2;
        }
    }

    // Package entries only. A std BMI is shared by every project on the machine
    // and costs ~30 s to rebuild; evicting one to hit a size target trades a lot
    // of time for a little disk. `cache clean --std` remains the explicit way.
    auto entries = walk_pkg_entries();
    std::ranges::sort(entries, [](const Entry& a, const Entry& b) {
        return a.accessed < b.accessed;      // oldest first
    });

    int removed = 0;
    std::uintmax_t freed = 0;
    std::uintmax_t live = 0;
    for (auto& e : entries) live += e.size;

    auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();

    for (auto& e : entries) {
        bool tooOld = olderThan && e.accessed != 0
                                && (now - e.accessed) > *olderThan;
        bool overBudget = maxSize && live > *maxSize;
        if (!tooOld && !overBudget) continue;
        remove_entry(e);
        ++removed;
        freed += e.size;
        live  -= e.size;
        mcpp::ui::status("Collected",
            std::format("{} ({}, last used {})",
                        e.label, human_bytes(e.size), format_age(e.accessed)));
    }
    std::println("");
    // "package entries", not "cache": `live` only ever counted package entries,
    // because std entries are deliberately out of scope here. Reporting it as
    // the cache size would read as "the cache is now empty" while tens of MB of
    // std BMIs sit right next to it.
    std::println("Collected {} entries, freed {} (package entries now {})",
                 removed, human_bytes(freed), human_bytes(live));
    if (maxSize && live > *maxSize) {
        // Say it rather than silently under-delivering: a size target that
        // cannot be met without evicting std BMIs is a real outcome, and a
        // caller that thinks the budget held would be misled.
        mcpp::ui::warning(std::format(
            "still {} over the {} budget: package entries are exhausted. "
            "std BMIs are excluded from gc; use `mcpp cache clean --std`",
            human_bytes(live - *maxSize), human_bytes(*maxSize)));
    }
    return 0;
}

// `mcpp cache clean [--deps] [--std] [--all] [--legacy]`.
export int cache_clean(bool deps, bool stds, bool all, bool legacy) {
    if (all) { deps = true; stds = true; }
    if (!deps && !stds && !legacy) deps = true;   // bare `clean` = dep entries

    std::error_code ec;
    if (deps) {
        auto n = dir_size(pkg_root());
        std::filesystem::remove_all(pkg_root(), ec);
        std::println("Cleaned package entries ({})", human_bytes(n));
    }
    if (stds) {
        auto n = dir_size(std_root());
        std::filesystem::remove_all(std_root(), ec);
        std::println("Cleaned std module entries ({})", human_bytes(n));
    }
    if (legacy) {
        auto legacyRoot = mcpp::home::legacy_bmi_root();
        if (std::filesystem::exists(legacyRoot, ec)) {
            auto n = dir_size(legacyRoot);
            std::filesystem::remove_all(legacyRoot, ec);
            std::println("Removed pre-v1 cache {} ({})", legacyRoot.string(),
                         human_bytes(n));
        } else {
            std::println("No pre-v1 cache at {}", legacyRoot.string());
        }
    }
    return 0;
}

// `mcpp cache verify` — entry.json against the disk, for every entry.
export int cache_verify() {
    auto entries = walk_pkg_entries(/*verify=*/true);
    auto stds    = walk_std_entries();
    int bad = 0;
    for (auto* set : {&entries, &stds}) {
        for (auto& e : *set) {
            if (e.complete) continue;
            ++bad;
            mcpp::ui::error(std::format("{}  [{}]  {}",
                                        e.label, e.key, e.problem));
            std::println("       {}", e.dir.string());
        }
    }
    auto total = entries.size() + stds.size();
    if (bad == 0) {
        std::println("{} entries verified, all complete", total);
        return 0;
    }
    std::println("");
    std::println("{} of {} entries are incomplete. They are treated as misses "
                 "and rebuilt; `mcpp cache gc --older-than 0s` or "
                 "`mcpp cache clean` reclaims them.", bad, total);
    return 1;
}

} // namespace mcpp::bmi_cache
