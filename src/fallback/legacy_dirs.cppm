// mcpp.fallback.legacy_dirs — legacy xpkg directory scan.
//
// Last-resort fallback scan (COMPAT, remove in 1.0.0): walk xpkgs/
// for any directory ending with -x-<qualifiedName> or -x-<shortName>.

export module mcpp.fallback.legacy_dirs;

import std;

export namespace mcpp::fallback {

// Scan the xpkgs base directory for a legacy install directory whose
// name ends with "-x-<qualifiedName>" or "-x-<shortName>".
// Returns the matching directory name (not the full path) if found.
std::optional<std::string>
scan_legacy_install_dirs(const std::filesystem::path& xpkgsBase,
                         std::string_view qualifiedName,
                         std::string_view shortName) {
    std::error_code ec;
    std::string suffix1 = std::format("-x-{}", qualifiedName);
    std::string suffix2 = std::format("-x-{}", shortName);

    for (auto& entry : std::filesystem::directory_iterator(xpkgsBase, ec)) {
        if (!entry.is_directory()) continue;
        auto dirname = entry.path().filename().string();
        if (dirname.ends_with(suffix1))
            return dirname;
        if (suffix2 != suffix1 && dirname.ends_with(suffix2))
            return dirname;
    }
    return std::nullopt;
}

} // namespace mcpp::fallback
