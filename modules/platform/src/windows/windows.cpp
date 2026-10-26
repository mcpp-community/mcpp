// mcpp.platform.windows on Windows.
module;
#include <cstdlib>
#include <stdlib.h>    // _putenv_s
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>   // GetACP, RegGetValueW
// RegGetValueW lives in advapi32, which a link line names only by default on
// MinGW; this asks MSVC-ABI links for it too.
#pragma comment(lib, "advapi32")

module mcpp.platform.windows;

import std;

namespace mcpp::platform::windows {
bool writable_fixed_drive(char drive, std::wstring_view child) {
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
}
}

// ─── Implementation ──────────────────────────────────────────────────────

namespace mcpp::platform::windows {

void prepend_path(const std::filesystem::path& dir) {
    std::string newPath = dir.string() + ";" +
        (std::getenv("PATH") ? std::getenv("PATH") : "");
    _putenv_s("PATH", newPath.c_str());
}

std::optional<std::filesystem::path> machine_registry_path(std::wstring_view subkey,
                                                           std::wstring_view value) {
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
}

bool long_paths_enabled() {
    DWORD value = 0, bytes = sizeof(value);
    return RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\FileSystem",
                        L"LongPathsEnabled", RRF_RT_REG_DWORD, nullptr, &value, &bytes) == ERROR_SUCCESS
        && value == 1;
}

std::optional<std::filesystem::path> env_directory(const char* name) {
    if (const char* v = std::getenv(name); v && *v) return std::filesystem::path(v);
    return std::nullopt;
}

unsigned active_code_page() {
    return static_cast<unsigned>(GetACP());
}

} // namespace mcpp::platform::windows
