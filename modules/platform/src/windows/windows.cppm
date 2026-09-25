// mcpp.platform.windows — Windows-specific platform capabilities.
//
// Provides:
//   prepend_path()      — add a directory to the front of %PATH%
//   active_code_page()  — the process ANSI code page (GetACP)
//
// Note: Visual Studio / MSVC discovery is in mcpp.toolchain.msvc, which is
// the authoritative module for MSVC toolchain detection.  This module
// provides general-purpose Windows platform utilities only.

module;
#include <cstdlib>
#if defined(_WIN32)
#include <stdlib.h>    // _putenv_s
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>   // GetACP
#endif

export module mcpp.platform.windows;

import std;

export namespace mcpp::platform::windows {

// Prepend a directory to the %PATH% environment variable.
void prepend_path(const std::filesystem::path& dir);

// The ANSI code page of this process: 65001 when the UTF-8 code page that
// mcpp.exe declares is in effect (Windows 10 version 1903 and later), the
// system's legacy code page otherwise. 0 on every other platform, which has
// no process code page. Every narrow string mcpp exchanges with Win32 is in
// this code page (#693).
unsigned active_code_page();

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

unsigned active_code_page() {
#if defined(_WIN32)
    return static_cast<unsigned>(GetACP());
#else
    return 0;
#endif
}

} // namespace mcpp::platform::windows
