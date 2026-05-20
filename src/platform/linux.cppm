// mcpp.platform.linux — Linux-specific platform capabilities.
//
// Provides:
//   build_ld_library_path()  — construct LD_LIBRARY_PATH prefix
//   runtime_lib_dirs()       — Linux-specific library search paths

module;

export module mcpp.platform.linux;

import std;
import mcpp.platform.shell;

export namespace mcpp::platform::linux_ {

// Build an LD_LIBRARY_PATH shell prefix from a list of directories.
// Returns "env LD_LIBRARY_PATH=<dirs>:${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH} "
// or "" if dirs is empty.
std::string build_ld_library_path_prefix(
    const std::vector<std::filesystem::path>& dirs);

// Return Linux-specific runtime library directories for LLVM toolchains.
std::vector<std::filesystem::path>
runtime_lib_dirs(const std::filesystem::path& toolchain_root);

} // namespace mcpp::platform::linux_

// ─── Implementation ──────────────────────────────────────────────────────

namespace mcpp::platform::linux_ {

std::string build_ld_library_path_prefix(
    const std::vector<std::filesystem::path>& dirs)
{
#if defined(__linux__)
    if (dirs.empty()) return "";
    std::string joined;
    for (auto& d : dirs) {
        if (!joined.empty()) joined += ':';
        joined += d.string();
    }
    return std::format("env LD_LIBRARY_PATH={}${{LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}} ",
                       mcpp::platform::shell::quote(joined));
#else
    (void)dirs;
    return "";
#endif
}

std::vector<std::filesystem::path>
runtime_lib_dirs(const std::filesystem::path& toolchain_root) {
    std::vector<std::filesystem::path> dirs;
#if defined(__linux__)
    auto add = [&](const std::filesystem::path& p) {
        if (std::filesystem::exists(p))
            dirs.push_back(p);
    };
    add(toolchain_root / "lib" / "x86_64-unknown-linux-gnu");
    add("/lib/x86_64-linux-gnu");
    add("/usr/lib/x86_64-linux-gnu");
    add("/usr/lib64");
#else
    (void)toolchain_root;
#endif
    return dirs;
}

} // namespace mcpp::platform::linux_
