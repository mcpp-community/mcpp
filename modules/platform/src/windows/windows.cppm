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
#include <windows.h>   // GetACP, RegGetValueW
// RegGetValueW lives in advapi32, which a link line names only by default on
// MinGW; this asks MSVC-ABI links for it too.
#pragma comment(lib, "advapi32")
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

// A string value under HKEY_LOCAL_MACHINE, read from both registry views (the
// 64-bit one first, then WOW6432Node's), as a path. An installer records its
// installation directory there wherever it put it -- the Windows SDK's
// `Installed Roots\KitsRoot10` -- so a search that knows only `C:\Program
// Files` misses an SDK on another drive (D15). nullopt when absent and on
// every other platform.
std::optional<std::filesystem::path> machine_registry_path(std::wstring_view subkey,
                                                           std::wstring_view value);

// A directory a known-folder environment variable names (`ProgramFiles`,
// `ProgramFiles(x86)`), or nullopt. The system drive is not always `C:`.
std::optional<std::filesystem::path> env_directory(const char* name);

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

std::optional<std::filesystem::path> machine_registry_path(std::wstring_view subkey,
                                                           std::wstring_view value) {
#if defined(_WIN32)
    const std::wstring key(subkey), name(value);
    for (DWORD view : {static_cast<DWORD>(KEY_WOW64_64KEY), static_cast<DWORD>(KEY_WOW64_32KEY)}) {
        DWORD bytes = 0;
        if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), name.c_str(),
                         RRF_RT_REG_SZ | view, nullptr, nullptr, &bytes) != ERROR_SUCCESS
            || bytes < sizeof(wchar_t))
            continue;
        std::wstring buf(bytes / sizeof(wchar_t), L'\0');
        if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), name.c_str(),
                         RRF_RT_REG_SZ | view, nullptr, buf.data(), &bytes) != ERROR_SUCCESS)
            continue;
        while (!buf.empty() && (buf.back() == L'\0' || buf.back() == L'\\'))
            buf.pop_back();
        if (!buf.empty()) return std::filesystem::path(buf);
    }
    return std::nullopt;
#else
    (void)subkey; (void)value;
    return std::nullopt;
#endif
}

std::optional<std::filesystem::path> env_directory(const char* name) {
    if (const char* v = std::getenv(name); v && *v) return std::filesystem::path(v);
    return std::nullopt;
}

unsigned active_code_page() {
#if defined(_WIN32)
    return static_cast<unsigned>(GetACP());
#else
    return 0;
#endif
}

} // namespace mcpp::platform::windows
