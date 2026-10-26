// mcpp.platform.windows on Linux and macOS: what the Windows capabilities answer where there is no Windows.
module;
#include <cstdlib>

module mcpp.platform.windows;

import std;

namespace mcpp::platform::windows {
bool writable_fixed_drive(char drive, std::wstring_view child) {
    (void)drive;
    (void)child;
    return false;
}
}

// ─── Implementation ──────────────────────────────────────────────────────

namespace mcpp::platform::windows {

void prepend_path(const std::filesystem::path& dir) {
    (void)dir;
}

std::optional<std::filesystem::path> machine_registry_path(std::wstring_view subkey,
                                                           std::wstring_view value) {
    (void)subkey; (void)value;
    return std::nullopt;
}

bool long_paths_enabled() {
    return false;
}

std::optional<std::filesystem::path> env_directory(const char* name) {
    if (const char* v = std::getenv(name); v && *v) return std::filesystem::path(v);
    return std::nullopt;
}

unsigned active_code_page() {
    return 0;
}

} // namespace mcpp::platform::windows
