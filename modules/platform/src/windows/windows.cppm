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

// Whether the system lets a program that declares itself `longPathAware` use
// paths past MAX_PATH (260): HKLM\SYSTEM\CurrentControlSet\Control\FileSystem
// `LongPathsEnabled` = 1. false when it is 0 or absent, and on every other
// platform, which has no such limit.
bool long_paths_enabled();
// Used only by the first-run Windows home suggestion; never creates the
// actual home. An existing child must be a plain directory: inspecting its
// reparse-point attributes avoids following a potentially remote target.
// Other platforms answer false without touching a filesystem.
bool writable_fixed_drive(char drive, std::wstring_view child);
bool input_is_terminal();

} // namespace mcpp::platform::windows

namespace mcpp::platform::windows {
bool input_is_terminal() {
#if defined(_WIN32)
    DWORD mode = 0;
    return GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode) != 0;
#else
    return false;
#endif
}

bool writable_fixed_drive(char drive, std::wstring_view child) {
#if defined(_WIN32)
    const wchar_t root[] = {static_cast<wchar_t>(drive), L':', L'\\', L'\0'};
    if (GetDriveTypeW(root) != DRIVE_FIXED) return false;
    const auto candidate = std::filesystem::path(root) / child;
    const auto attributes = GetFileAttributesW(candidate.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const auto error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return false;
    } else if (!(attributes & FILE_ATTRIBUTE_DIRECTORY)
               || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    const auto probe = std::filesystem::path(root)
        / std::format(".mcpp-write-{}-{}", GetCurrentProcessId(), GetTickCount64());
    const auto handle = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    CloseHandle(handle);
    return true;
#else
    (void)drive;
    (void)child;
    return false;
#endif
}
}

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

bool long_paths_enabled() {
#if defined(_WIN32)
    DWORD value = 0, bytes = sizeof(value);
    return RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\FileSystem",
                        L"LongPathsEnabled", RRF_RT_REG_DWORD, nullptr, &value, &bytes) == ERROR_SUCCESS
        && value == 1;
#else
    return false;
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
