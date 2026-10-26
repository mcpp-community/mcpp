// mcpp.platform.fs on Windows: MoveFileExW's replacement, FILE_APPEND_DATA,
// and LockFileEx.
module;
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

module mcpp.platform.fs;

import std;

namespace mcpp::platform::fs {

bool replace_file(const std::filesystem::path& source,
                  const std::filesystem::path& destination,
                  std::error_code& ec) {
    // 直接替换已有文件，不能先删除 last-known-good CDB。编辑器或杀毒软件
    // 可能短暂占用目标文件，sharing violation 仅做有限退避后再报告失败。
    auto delay = std::chrono::milliseconds{50};
    for (int attempt = 0; attempt < 4; ++attempt) {
        if (MoveFileExW(source.wstring().c_str(), destination.wstring().c_str(),
                        MOVEFILE_REPLACE_EXISTING)) {
            ec.clear();
            return true;
        }
        const auto error = GetLastError();
        if (error != ERROR_SHARING_VIOLATION || attempt == 3) {
            ec = std::error_code(static_cast<int>(error), std::system_category());
            return false;
        }
        std::this_thread::sleep_for(delay);
        delay *= 3;
    }
    return false;
}

bool append_atomically(const std::filesystem::path& file, std::string_view bytes) {
    HANDLE h = CreateFileW(file.wstring().c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()),
                              &written, nullptr) && written == bytes.size();
    CloseHandle(h);
    return ok;
}

// ── FileLock ──

namespace {

HANDLE handle_of(std::intptr_t native) { return reinterpret_cast<HANDLE>(native); }

void release(std::intptr_t native) {
    if (handle_of(native) == INVALID_HANDLE_VALUE) return;
    OVERLAPPED ov = {};
    UnlockFileEx(handle_of(native), 0, 1, 0, &ov);
    CloseHandle(handle_of(native));
}

} // namespace

std::optional<FileLock> FileLock::try_acquire(const std::filesystem::path& dir,
                                              std::string_view name) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    auto lockPath = dir / std::string(name);
    HANDLE h = CreateFileW(lockPath.wstring().c_str(),
        GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return std::nullopt;
    OVERLAPPED ov = {};
    if (!LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                    0, 1, 0, &ov)) {
        CloseHandle(h);
        return std::nullopt;
    }
    return FileLock{reinterpret_cast<std::intptr_t>(h)};
}

FileLock::~FileLock() {
    release(native_);
}

FileLock::FileLock(FileLock&& o) noexcept : native_(o.native_) {
    o.native_ = -1;
}

FileLock& FileLock::operator=(FileLock&& o) noexcept {
    if (this != &o) {
        release(native_);
        native_ = o.native_;
        o.native_ = -1;
    }
    return *this;
}

} // namespace mcpp::platform::fs
