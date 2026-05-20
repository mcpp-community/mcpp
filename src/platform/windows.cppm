// mcpp.platform.windows — Windows-specific platform capabilities.
//
// Provides:
//   prepend_path()  — add a directory to the front of %PATH%
//
// Note: Visual Studio / MSVC discovery is in mcpp.toolchain.msvc, which is
// the authoritative module for MSVC toolchain detection.  This module
// provides general-purpose Windows platform utilities only.

module;
#include <cstdlib>
#if defined(_WIN32)
#include <stdlib.h>    // _putenv_s
#endif

export module mcpp.platform.windows;

import std;

export namespace mcpp::platform::windows {

// Prepend a directory to the %PATH% environment variable.
void prepend_path(const std::filesystem::path& dir);

} // namespace mcpp::platform::windows

// ─── Implementation ──────────────────────────────────────────────────────

namespace mcpp::platform::windows {

void prepend_path(const std::filesystem::path& dir) {
#if defined(_WIN32)
    std::string newPath = dir.string() + ";" +
        (std::getenv("PATH") ? std::getenv("PATH") : "");
    _putenv_s("PATH", newPath.c_str());
#else
    (void)dir;
#endif
}

} // namespace mcpp::platform::windows
