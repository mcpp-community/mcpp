// mcpp.bmi_cache.maintenance — global BMI cache inspection + pruning, and the
// shared fs-size/byte-formatting helpers they are built on.
// Bodies moved verbatim from the CLI layer. Zero behavior change.

module;
#include <cstdio>
#include <cstdlib>

export module mcpp.bmi_cache.maintenance;

import std;
import mcpp.toolchain.stdmod;
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


// ─── M4 #4: mcpp cache list / prune / clean / info ──────────────────────
struct CacheEntry {
    std::filesystem::path           dir;
    std::string                     fingerprint;
    std::string                     pkgAtVer;        // "<idx>/<pkg>@<ver>"
    std::uintmax_t                  size = 0;
    std::filesystem::file_time_type lastWrite{};
    std::size_t                     fileCount = 0;
};

static std::vector<CacheEntry> walk_cache_entries() {
    std::vector<CacheEntry> entries;
    auto bmi = mcpp::toolchain::default_cache_root();
    std::error_code ec;
    if (!std::filesystem::exists(bmi, ec)) return entries;

    for (auto& fpEntry : std::filesystem::directory_iterator(bmi, ec)) {
        auto fpDir = fpEntry.path();
        auto depsDir = fpDir / "deps";
        if (!std::filesystem::exists(depsDir, ec)) continue;
        for (auto& idxEntry : std::filesystem::directory_iterator(depsDir, ec)) {
            for (auto& pkgEntry : std::filesystem::directory_iterator(idxEntry.path(), ec)) {
                CacheEntry e;
                e.dir         = pkgEntry.path();
                e.fingerprint = fpDir.filename().string();
                e.pkgAtVer    = idxEntry.path().filename().string()
                              + "/" + pkgEntry.path().filename().string();
                e.size        = dir_size(e.dir);
                e.lastWrite   = std::filesystem::last_write_time(e.dir, ec);
                for (auto& _ : std::filesystem::recursive_directory_iterator(e.dir, ec)) {
                    if (!ec) ++e.fileCount;
                }
                entries.push_back(std::move(e));
            }
        }
    }
    return entries;
}

static std::string format_age(std::filesystem::file_time_type t) {
    auto now = std::chrono::file_clock::now();
    auto diff = std::chrono::duration_cast<std::chrono::seconds>(now - t).count();
    if (diff < 60)        return std::format("{}s ago", diff);
    if (diff < 3600)      return std::format("{}m ago", diff / 60);
    if (diff < 86400)     return std::format("{}h ago", diff / 3600);
    return std::format("{}d ago", diff / 86400);
}

// `mcpp cache` is dispatched at the App level — list / info / prune / clean
// each get their own action lambda invoking one of these helpers.

// `mcpp cache list`.
export int cache_list() {
    auto entries = walk_cache_entries();
    if (entries.empty()) {
        std::println("(BMI cache is empty)");
        return 0;
    }
    std::println("{:<18}  {:>10}  {:>14}  {}",
                 "fingerprint", "size", "last accessed", "package");
    for (auto& e : entries) {
        auto fp = e.fingerprint.size() > 16
                  ? e.fingerprint.substr(0, 16) : e.fingerprint;
        std::println("{:<18}  {:>10}  {:>14}  {}",
            fp, human_bytes(e.size), format_age(e.lastWrite), e.pkgAtVer);
    }
    return 0;
}

// `mcpp cache info <pkg>@<ver>`.
export int cache_info(const std::string& needle) {
    auto entries = walk_cache_entries();
    for (auto& e : entries) {
        if (e.pkgAtVer.ends_with(needle)) {
            std::println("dir          = {}", e.dir.string());
            std::println("fingerprint  = {}", e.fingerprint);
            std::println("package      = {}", e.pkgAtVer);
            std::println("size         = {}", human_bytes(e.size));
            std::println("file count   = {}", e.fileCount);
            std::println("last write   = {}", format_age(e.lastWrite));
            return 0;
        }
    }
    std::println("no cache entry matching '{}'", needle);
    return 1;
}

// `mcpp cache prune --older-than <N>{s,m,h,d}` (v = raw option value).
export int cache_prune(const std::string& v) {
    if (v.empty()) {
        mcpp::ui::error("`mcpp cache prune` requires --older-than <N>{s,m,h,d}");
        return 2;
    }
    char unit = v.back();
    long long n = 0;
    try { n = std::stoll(v.substr(0, v.size() - 1)); }
    catch (...) { mcpp::ui::error(std::format("bad --older-than value '{}'", v)); return 2; }
    std::chrono::seconds threshold{0};
    if      (unit == 's') threshold = std::chrono::seconds(n);
    else if (unit == 'm') threshold = std::chrono::seconds(n * 60);
    else if (unit == 'h') threshold = std::chrono::seconds(n * 3600);
    else if (unit == 'd') threshold = std::chrono::seconds(n * 86400);
    else { mcpp::ui::error(std::format("bad time unit '{}': use s/m/h/d", unit)); return 2; }
    auto cutoff = std::chrono::file_clock::now() - threshold;
    auto entries = walk_cache_entries();
    int removed = 0;
    std::uintmax_t freed = 0;
    for (auto& e : entries) {
        if (e.lastWrite < cutoff) {
            std::error_code ec;
            std::filesystem::remove_all(e.dir, ec);
            if (!ec) {
                ++removed;
                freed += e.size;
                mcpp::ui::status("Pruned",
                    std::format("{} ({})", e.pkgAtVer, human_bytes(e.size)));
            }
        }
    }
    std::println("");
    std::println("Pruned {} entries, freed {}", removed, human_bytes(freed));
    return 0;
}

// `mcpp cache clean` — drop dep entries, preserve std BMIs.
export int cache_clean() {
    auto bmi = mcpp::toolchain::default_cache_root();
    std::error_code ec;
    std::filesystem::remove_all(bmi / "deps", ec);   // deps only; preserve std.gcm
    if (std::filesystem::exists(bmi)) {
        for (auto& f : std::filesystem::directory_iterator(bmi, ec)) {
            auto deps = f.path() / "deps";
            std::filesystem::remove_all(deps, ec);
        }
    }
    std::println("Cleaned all dep BMI cache entries (std.gcm preserved)");
    return 0;
}

} // namespace mcpp::bmi_cache
