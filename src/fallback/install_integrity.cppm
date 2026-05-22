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

// Check whether an xpkg directory looks like a complete installation.
// Returns true if:
//   - .mcpp_ok marker exists, OR
//   - (backward compat) bin/, lib/, or include/ exists
bool is_install_complete(const std::filesystem::path& xpkgDir);

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

} // namespace mcpp::fallback

// ─── Implementation ─────────────────────────────────────────────────

namespace mcpp::fallback {

namespace {

// Heuristic: does this directory look like a complete xpkg?
// Used ONLY for legacy migration (pre-.mcpp_ok packages).
bool looks_complete_legacy(const std::filesystem::path& xpkgDir) {
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

} // namespace

// Strict: has .mcpp_ok marker (written only on verified success).
bool has_marker(const std::filesystem::path& xpkgDir) {
    return std::filesystem::exists(xpkgDir / std::string(kInstallMarker));
}

bool is_install_complete(const std::filesystem::path& xpkgDir) {
    if (!std::filesystem::exists(xpkgDir)) return false;

    // Primary: .mcpp_ok marker — the only fully trusted signal.
    if (has_marker(xpkgDir)) return true;

    // Read-only legacy fallback: recognize pre-upgrade packages so they
    // aren't treated as missing by resolve_xpkg_path(). Does NOT write
    // any marker. Does NOT prevent clean_incomplete_install() from
    // cleaning — that function uses marker-only semantics.
    return looks_complete_legacy(xpkgDir);
}

void mark_install_complete(const std::filesystem::path& xpkgDir) {
    auto marker = xpkgDir / std::string(kInstallMarker);
    if (std::filesystem::exists(marker)) return;
    std::ofstream ofs(marker);
    if (ofs) ofs << "1\n";
}

bool clean_incomplete_install(const std::filesystem::path& xpkgDir) {
    if (!std::filesystem::exists(xpkgDir)) return false;

    // Marker-only: if .mcpp_ok exists, this is a verified install — keep it.
    // Legacy packages (no marker but has content) are NOT cleaned here;
    // they're recognized by is_install_complete() for read-only compat.
    if (has_marker(xpkgDir)) return false;

    // No marker. If it looks like a legacy complete package, don't clean
    // it either — it predates the marker system.
    if (looks_complete_legacy(xpkgDir)) {
        mcpp::log::debug("integrity", std::format(
            "legacy package without marker, skipping cleanup: {}",
            xpkgDir.string()));
        return false;
    }

    // No marker, no legacy content — this is genuinely incomplete.
    mcpp::log::verbose("integrity",
        std::format("cleaning incomplete install: {}", xpkgDir.string()));
    std::error_code ec;
    std::filesystem::remove_all(xpkgDir, ec);
    return !ec;
}

int clean_all_incomplete(const std::filesystem::path& xpkgsBase) {
    if (!std::filesystem::exists(xpkgsBase)) return 0;

    int cleaned = 0;
    std::error_code ec;
    for (auto& pkgDir : std::filesystem::directory_iterator(xpkgsBase, ec)) {
        if (!pkgDir.is_directory()) continue;
        for (auto& verDir : std::filesystem::directory_iterator(pkgDir.path(), ec)) {
            if (!verDir.is_directory()) continue;
            if (clean_incomplete_install(verDir.path()))
                ++cleaned;
        }
    }
    return cleaned;
}


} // namespace mcpp::fallback
