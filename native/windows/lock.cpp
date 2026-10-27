// The lock every release of mcpp takes on Windows: LockFileEx on the first byte.
module;
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

module mcpp.home.files;

import std;

namespace mcpp::home::native {

std::intptr_t lock_file(const std::filesystem::path& file) {
    HANDLE h = CreateFileW(file.wstring().c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return -1;
    OVERLAPPED ov = {};
    if (!LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &ov)) {
        CloseHandle(h);
        return -1;
    }
    return reinterpret_cast<std::intptr_t>(h);
}

void unlock_file(std::intptr_t handle) {
    OVERLAPPED ov = {};
    UnlockFileEx(reinterpret_cast<HANDLE>(handle), 0, 1, 0, &ov);
    CloseHandle(reinterpret_cast<HANDLE>(handle));
}

}  // namespace mcpp::home::native
