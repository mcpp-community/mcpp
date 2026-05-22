// mcpp.fallback.xpkg_copy — copy xpkg payload from global ~/.xlings/ to sandbox.
//
// Workaround: xlings may extract large packages (e.g. LLVM) into its
// global data dir instead of the mcpp sandbox, because the extraction
// subprocess doesn't always inherit XLINGS_HOME. This module detects
// the global copy and mirrors it into the sandbox so mcpp remains
// self-contained.

module;
#include <cstdlib>

export module mcpp.fallback.xpkg_copy;

import std;
import mcpp.log;

export namespace mcpp::fallback {

// Try to copy an xpkg verdir from the global xlings home into the
// sandbox. Returns true if the copy succeeded (verdir now exists).
bool copy_xpkg_from_global(const std::filesystem::path& sandboxVerdir) {
    if (std::filesystem::exists(sandboxVerdir)) return true;

    mcpp::log::verbose("fallback", "verdir not in sandbox, checking global xlings");

    const char* xhome = nullptr;
#if defined(_WIN32)
    xhome = std::getenv("USERPROFILE");
#endif
    if (!xhome) xhome = std::getenv("HOME");
    if (!xhome) return false;

    mcpp::log::debug("fallback", std::format("HOME={}", xhome));

    // xlings stores xpkgs at <home>/.xlings/data/xpkgs/ or
    // <home>/.xlings/subos/default/data/xpkgs/
    auto pkgDir = sandboxVerdir.parent_path().filename().string();
    auto verName = sandboxVerdir.filename().string();
    std::filesystem::path candidates[] = {
        std::filesystem::path(xhome) / ".xlings" / "data" / "xpkgs" / pkgDir / verName,
        std::filesystem::path(xhome) / ".xlings" / "subos" / "default" / "data" / "xpkgs" / pkgDir / verName,
    };

    for (auto& src : candidates) {
        std::error_code ec;
        bool srcExists = std::filesystem::exists(src, ec) && std::filesystem::is_directory(src, ec);
        mcpp::log::debug("fallback", std::format(
            "candidate '{}' exists={}", src.string(), srcExists));
        if (srcExists) {
            std::filesystem::create_directories(sandboxVerdir.parent_path(), ec);
            std::filesystem::copy(src, sandboxVerdir,
                std::filesystem::copy_options::recursive
                | std::filesystem::copy_options::overwrite_existing, ec);
            mcpp::log::verbose("fallback", std::format(
                "copied from global xlings: ec={}", ec.message()));
            if (!ec) return true;
        }
    }
    return false;
}

} // namespace mcpp::fallback
