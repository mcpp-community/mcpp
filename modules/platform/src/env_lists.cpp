// The lists of directories mcpp composes for a child's environment, over mcpplibs.os's
// conventions (the host's separator, an entry quoted whole on Windows). An implementation unit, so
// the interface of mcpp.platform.env imports nothing beyond what it declares.
module mcpp.platform.env;

import std;
import mcpplibs.os;

namespace mcpp::platform::env {

std::string path_list_separator() {
    return std::string(1, mcpplibs::os::conventions().path_list_separator);
}

std::string strip_private_glibc(std::string_view paths) {
    std::vector<std::filesystem::path> kept;
    for (auto& item : mcpplibs::os::env::split_list(paths)) {
        const auto text = item.generic_string();
        if (text.find("/xim-x-glibc/") == std::string::npos) kept.push_back(std::move(item));
    }
    return mcpplibs::os::env::join_list(kept);
}

std::string prepend_path_list(std::string_view key,
                              std::span<const std::filesystem::path> dirs) {
    if (key.empty() || dirs.empty()) return "";
    std::vector<std::filesystem::path> given;
    for (auto& dir : dirs) if (!dir.empty()) given.push_back(dir);
    std::string value = mcpplibs::os::env::join_list(given);
    if (value.empty()) return "";

    std::string k(key);
    if (auto* existing = std::getenv(k.c_str()); existing && *existing) {
        // Loader paths only: the inherited value may carry an OUTER mcpp's
        // private-glibc entry, and the composed string becomes an EXPLICIT
        // override — which process.cppm's merged_environ takes verbatim,
        // skipping the sanitation it applies to inherited variables. Without
        // this the strip is defeated exactly when it matters (a nested
        // `mcpp run` → `mcpp test` chain), and the child tool segfaults in the
        // dynamic linker whenever the payload versions differ.
        std::string tail = (k == "LD_LIBRARY_PATH" || k == "DYLD_LIBRARY_PATH")
            ? strip_private_glibc(existing) : std::string(existing);
        if (!tail.empty()) {
            value += path_list_separator();
            value += tail;
        }
    }
    return value;
}

} // namespace mcpp::platform::env
