// mcpp.fallback.legacy_dirs — legacy xpkg directory scan.
//
// Last-resort fallback scan (COMPAT, remove in 1.0.0): walk xpkgs/ for a
// directory that holds the requested package under an older naming layout.
//
// WHY THE SCAN IS NAMESPACE-BOUND
//
// The bare `-x-<shortName>` arm used to match ANY prefix, so a lookup for
// `ocornut:imgui` happily returned `compat-x-imgui` — a DIFFERENT package that
// merely shares a short name. `Fetcher::install_path` then treats that verdir
// as the requested package's, which means:
//
//   * the install is skipped (the package "already exists"), and
//   * whatever is inside the other namespace's verdir is what gets read.
//
// It stayed invisible while the two packages carried unrelated versions —
// `compat:imgui@1.92.8` next to `mcpplibs:imgui@0.0.6` never collide, because
// install_path also matches on version. Aligning package versions to upstream
// (mcpp-index#163: `imgui@0.0.6` really was ImGui 1.92.8) makes them coincide,
// and the bug becomes reachable.
//
// The observed failure was loud only by luck: `compat.imgui` is a Form B
// descriptor, so the wrong verdir had no mcpp.toml and the build stopped with
// "index entry has no `mcpp = ...` field" — a diagnostic naming the wrong
// cause. Between two Form A packages the wrong verdir DOES have a manifest,
// and the build would have silently compiled the wrong package.
//
// So the shortName arm now requires the directory's own namespace prefix to be
// one the caller actually asked for. That keeps the legacy layouts it exists
// for (`<ns>-x-<shortName>`, `<index>-x-<shortName>`) and refuses the one it
// was never meant to serve: some other namespace's package.

export module mcpp.fallback.legacy_dirs;

import std;

export namespace mcpp::fallback {

// Scan the xpkgs base directory for a legacy install directory holding
// (namespace, shortName). Returns the matching directory name (not the full
// path) if found.
//
// `acceptedPrefixes` are the directory prefixes (the part before `-x-`) that
// may satisfy a bare short-name match — the requested namespace, and the index
// name for the old index-prefixed layout. A fully-qualified `-x-<ns>.<name>`
// match carries the namespace in the suffix itself and needs no prefix check.
std::optional<std::string>
scan_legacy_install_dirs(const std::filesystem::path& xpkgsBase,
                         std::string_view qualifiedName,
                         std::string_view shortName,
                         const std::vector<std::string>& acceptedPrefixes) {
    std::error_code ec;
    std::string suffix1 = std::format("-x-{}", qualifiedName);
    std::string suffix2 = std::format("-x-{}", shortName);

    auto prefix_accepted = [&](const std::string& dirname) {
        auto cut = dirname.size() - suffix2.size();
        std::string_view prefix{dirname.data(), cut};
        for (auto& p : acceptedPrefixes)
            if (prefix == p) return true;
        return false;
    };

    for (auto& entry : std::filesystem::directory_iterator(xpkgsBase, ec)) {
        if (!entry.is_directory()) continue;
        auto dirname = entry.path().filename().string();
        if (dirname.ends_with(suffix1))
            return dirname;
        if (suffix2 != suffix1 && dirname.ends_with(suffix2)
            && prefix_accepted(dirname))
            return dirname;
    }
    return std::nullopt;
}

} // namespace mcpp::fallback
