// mcpp.fallback.install_integrity — unified incomplete-install detection & cleanup.
//
// Every xpkg install (toolchain, bootstrap tool, modular lib) goes through
// the same lifecycle:
//
//   1. xlings creates the xpkg directory
//   2. downloads / extracts / elfpatches
//   3. mcpp writes `.mcpp_ok` marker on success
//
// If step 2 is interrupted (Ctrl+C, network failure, kill -9), the directory
// exists but is incomplete. This module provides a single mechanism to detect
// and clean up such residue, used by:
//
//   - resolve_xpkg_path()     (package_fetcher.cppm)
//   - ensure_patchelf/ninja() (xlings.cppm bootstrap)
//   - mcpp self init          (cli.cppm)
//
// Marker file: `.mcpp_ok` — written ONLY by mcpp after verified install.
// Absence of marker + directory exists = incomplete → safe to delete.
// Backward compat: packages installed before this feature have no marker;
// fall back to heuristic check (bin/ or lib/ or include/ exists).

module;
#include <cstdio>

export module mcpp.fallback.install_integrity;

import std;
import mcpp.log;

export namespace mcpp::fallback {

// Marker file name written into xpkg directories after successful install.
inline constexpr std::string_view kInstallMarker = ".mcpp_ok";

// Check whether an xpkg directory has the .mcpp_ok marker.
// STRICT marker-only — does not fall back to legacy heuristics.
bool is_install_complete(const std::filesystem::path& xpkgDir);

// Heuristic check for pre-.mcpp_ok packages (upgrade compat).
// Returns true if the directory looks like a complete legacy install
// based on layout (top-level bin/lib/lib64/include/share, or a single
// subdirectory containing src/ or mcpp.toml).
// Use ONLY for one-time legacy adoption or to avoid deleting old packages;
// do NOT use this to decide "skip install" on the active install path —
// that's what is_install_complete()'s strict semantics protect against.
bool looks_complete_legacy(const std::filesystem::path& xpkgDir);

// Write the .mcpp_ok marker into a directory, marking it as complete.
void mark_install_complete(const std::filesystem::path& xpkgDir);

// If xpkgDir exists but is incomplete, remove it entirely.
// Returns true if residue was cleaned (directory was removed).
// Returns false if directory doesn't exist or is already complete.
bool clean_incomplete_install(const std::filesystem::path& xpkgDir);

// Scan an xpkgs base directory and clean ALL incomplete installations.
// Only cleans directories without .mcpp_ok marker AND without legacy
// content (won't delete pre-upgrade packages).
// Used by `mcpp self init`.
// Returns number of directories cleaned.
int clean_all_incomplete(const std::filesystem::path& xpkgsBase);

// RAII rollback guard for the resolve/install path.
//
// Replaces a bare clean_incomplete_install() before a reinstall attempt.
// Instead of DELETING a no-marker directory outright (which permanently
// destroys a content-complete *legacy* package if the reinstall then fails —
// the xim-x-zlib regression), it RENAMES the directory aside and only commits
// to deleting it once the reinstall is known to have produced a good install.
//
// Usage:
//   InstallStash stash(verdir);   // moves verdir -> verdir.mcpp-stash (no marker)
//   ... attempt reinstall into verdir ...
//   stash.commit();               // reinstall produced a good install: drop backup
//   // On any return/throw WITHOUT commit(), the destructor:
//   //   - keeps the new install if verdir now has a marker (reinstall won);
//   //   - else, if the stashed dir looks_complete_legacy(), restores it and
//   //     marks it complete (a working package was wrongly cleaned);
//   //   - else discards it (genuine half-extracted residue).
class InstallStash {
public:
    explicit InstallStash(std::filesystem::path xpkgDir);
    ~InstallStash();
    InstallStash(const InstallStash&) = delete;
    InstallStash& operator=(const InstallStash&) = delete;

    // Reinstall succeeded: discard the backup, leave the live dir in place.
    void commit() noexcept;
    // Whether anything was actually stashed (dir existed and had no marker).
    bool stashed() const noexcept { return active_; }

private:
    std::filesystem::path orig_;
    std::filesystem::path stash_;
    bool active_    = false;
    bool committed_ = false;
};

} // namespace mcpp::fallback

// ─── Implementation ─────────────────────────────────────────────────

namespace mcpp::fallback {

bool looks_complete_legacy(const std::filesystem::path& xpkgDir) {
    if (!std::filesystem::exists(xpkgDir)) return false;
    // xim toolchain/tool packages: top-level bin/lib/lib64/include/share
    for (auto dir : {"bin", "lib", "lib64", "include", "share"}) {
        if (std::filesystem::exists(xpkgDir / dir))
            return true;
    }
    // mcpplibs layout: single subdirectory containing src/ or mcpp.toml
    std::error_code ec;
    std::vector<std::filesystem::path> subs;
    for (auto& e : std::filesystem::directory_iterator(xpkgDir, ec)) {
        if (e.is_directory()) subs.push_back(e.path());
    }
    if (subs.size() == 1) {
        auto& sub = subs[0];
        if (std::filesystem::exists(sub / "src")
         || std::filesystem::exists(sub / "mcpp.toml")
         || std::filesystem::exists(sub / "include")
         || std::filesystem::exists(sub / "bin"))
            return true;
    }
    return false;
}

// Strict: has .mcpp_ok marker (written only on verified success).
bool has_marker(const std::filesystem::path& xpkgDir) {
    return std::filesystem::exists(xpkgDir / std::string(kInstallMarker));
}

bool is_install_complete(const std::filesystem::path& xpkgDir) {
    if (!std::filesystem::exists(xpkgDir)) return false;

    // STRICT marker-only.
    // Used on the install/resolve path — half-extracted dirs with bin/
    // would otherwise be mistaken for complete packages.
    //
    // Legacy packages (installed before .mcpp_ok existed) will trigger
    // a one-time reinstall after upgrade. This is the cost of strict
    // semantics; the alternative (legacy heuristic) shields half-extracted
    // packages from cleanup and re-introduces the very bug we're fixing.
    // The reinstall is cheap because copy_xpkg_from_global() is the
    // typical fallback path — it reuses the existing ~/.xlings/ copy.
    return has_marker(xpkgDir);
}

void mark_install_complete(const std::filesystem::path& xpkgDir) {
    auto marker = xpkgDir / std::string(kInstallMarker);
    if (std::filesystem::exists(marker)) return;
    std::ofstream ofs(marker);
    if (ofs) ofs << "1\n";
}

bool clean_incomplete_install(const std::filesystem::path& xpkgDir) {
    if (!std::filesystem::exists(xpkgDir)) return false;

    // STRICT marker-only semantics.
    // Used on the resolve/install path for the CURRENT target: we know
    // mcpp just attempted to install this package, so absence of .mcpp_ok
    // unambiguously means the attempt was incomplete (interrupted, failed
    // mid-extract, etc.). Legacy heuristic compat does NOT apply here —
    // a half-extracted dir that happens to have a `bin/` would otherwise
    // escape cleanup and corrupt subsequent installs.
    if (has_marker(xpkgDir)) return false;

    mcpp::log::verbose("integrity",
        std::format("cleaning incomplete install: {}", xpkgDir.string()));
    std::error_code ec;
    std::filesystem::remove_all(xpkgDir, ec);
    return !ec;
}

int clean_all_incomplete(const std::filesystem::path& xpkgsBase) {
    if (!std::filesystem::exists(xpkgsBase)) return 0;

    // Global scan (used by `mcpp self init`). Keeps legacy packages
    // (no marker but has content) for backward compatibility — those
    // were installed before the marker system existed.
    int cleaned = 0;
    std::error_code ec;
    for (auto& pkgDir : std::filesystem::directory_iterator(xpkgsBase, ec)) {
        if (!pkgDir.is_directory()) continue;
        for (auto& verDir : std::filesystem::directory_iterator(pkgDir.path(), ec)) {
            if (!verDir.is_directory()) continue;
            if (has_marker(verDir.path())) continue;
            if (looks_complete_legacy(verDir.path())) {
                mcpp::log::debug("integrity", std::format(
                    "legacy package without marker, kept: {}",
                    verDir.path().string()));
                continue;
            }
            std::filesystem::remove_all(verDir.path(), ec);
            if (!ec) ++cleaned;
        }
    }
    return cleaned;
}

// ─── InstallStash ───────────────────────────────────────────────────

InstallStash::InstallStash(std::filesystem::path xpkgDir)
    : orig_(std::move(xpkgDir))
{
    std::error_code ec;
    if (!std::filesystem::exists(orig_, ec)) return;  // nothing to stash
    if (has_marker(orig_)) return;                    // complete: leave untouched

    stash_ = orig_;
    stash_ += ".mcpp-stash";
    std::filesystem::remove_all(stash_, ec);          // clear any prior stash residue
    std::filesystem::rename(orig_, stash_, ec);
    if (ec) {
        // Rename failed (e.g. cross-device); fall back to the historical
        // delete-then-reinstall semantics so the caller still starts clean.
        std::error_code rmec;
        std::filesystem::remove_all(orig_, rmec);
        stash_.clear();
        return;
    }
    active_ = true;
}

void InstallStash::commit() noexcept {
    committed_ = true;
    if (!active_) return;
    std::error_code ec;
    std::filesystem::remove_all(stash_, ec);
    active_ = false;
}

InstallStash::~InstallStash() {
    if (!active_ || committed_) return;
    std::error_code ec;

    // Reinstall produced a complete install at orig_ — keep it, drop backup.
    if (std::filesystem::exists(orig_, ec) && has_marker(orig_)) {
        std::filesystem::remove_all(stash_, ec);
        return;
    }

    if (looks_complete_legacy(stash_)) {
        // A content-complete legacy package (no marker) was cleaned for a
        // reinstall that then failed. Roll it back so we never leave a
        // working package deleted, and adopt it (write the marker) so the
        // next resolve trusts it instead of repeating the delete-then-fail.
        std::filesystem::remove_all(orig_, ec);            // clear partial residue
        std::error_code rnec;
        std::filesystem::rename(stash_, orig_, rnec);
        if (!rnec) {
            mark_install_complete(orig_);
            mcpp::log::verbose("integrity", std::format(
                "rolled back stashed legacy install after failed reinstall: {}",
                orig_.string()));
        } else {
            mcpp::log::verbose("integrity", std::format(
                "failed to roll back stashed install {} -> {}: {}",
                stash_.string(), orig_.string(), rnec.message()));
        }
    } else {
        // Genuine interrupted residue — discard (historical semantics).
        std::filesystem::remove_all(stash_, ec);
    }
}


} // namespace mcpp::fallback
