// mcpp.runtime.launch_env over mcpplibs.os's lists of directories (the host's
// separator, an entry quoted whole on Windows) and this invocation's
// environment.
module mcpp.runtime.launch_env;

import std;
import mcpplibs.os.platform;
import mcpplibs.os.env;
import mcpp.invocation;
import mcpp.provider;
import mcpp.provider.select;

namespace mcpp::runtime::launch_env {

namespace os = mcpplibs::os;

std::string path_list_separator() { return std::string(1, os::path_list_separator); }

const std::string& inherited_path() {
    static const std::string path = os::env::get("PATH").value_or("");
    return path;
}

std::string runtime_library_path_key() {
    if constexpr (os::is_windows) return "PATH";
    else if constexpr (os::is_linux) return "LD_LIBRARY_PATH";
    else return "";
}

std::string host_tool_runtime_library_path_key() {
    if constexpr (os::is_macos) return "DYLD_LIBRARY_PATH";
    else if constexpr (os::is_linux) return "LD_LIBRARY_PATH";
    else return "";
}

std::string strip_private_glibc(std::string_view paths) {
    std::vector<std::filesystem::path> kept;
    for (auto& item : os::env::split_list(paths)) {
        // A directory of the store's glibc payload, whatever the provider
        // spells the store: what the provider's layout says the path is.
        if (mcpp::provider::layout().entry_of(item).name != "glibc") kept.push_back(std::move(item));
    }
    return os::env::join_list(kept);
}

std::string prepend_path_list(std::string_view key, std::span<const std::filesystem::path> dirs) {
    if (key.empty() || dirs.empty()) return "";
    std::vector<std::filesystem::path> given;
    for (auto& dir : dirs) if (!dir.empty()) given.push_back(dir);
    std::string value = os::env::join_list(given);
    if (value.empty()) return "";
    if (auto existing = mcpp::invocation::get(key)) {
        // A loader path's inherited tail may carry an OUTER mcpp's private-glibc
        // entry, and the composed value is given to the child as stated --
        // skipping the sanitation mcpp.run applies to inherited variables.
        std::string tail = (key == "LD_LIBRARY_PATH" || key == "DYLD_LIBRARY_PATH")
            ? strip_private_glibc(*existing) : *existing;
        if (!tail.empty()) {
            value += path_list_separator();
            value += tail;
        }
    }
    return value;
}

}  // namespace mcpp::runtime::launch_env
