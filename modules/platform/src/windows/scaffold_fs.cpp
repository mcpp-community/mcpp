// mcpp.platform.scaffold_fs on Windows: FlushFileBuffers of a file; a
// directory's entry is already durable by MOVEFILE_WRITE_THROUGH.
module;
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

module mcpp.platform.scaffold_fs;

import std;

namespace mcpp::platform {

namespace {

std::string win32_message(DWORD code) {
    return std::system_category().message(static_cast<int>(code));
}

} // namespace

std::expected<void, std::string> sync_regular_file(const std::filesystem::path& path) {
    HANDLE handle = ::CreateFileW(
        path.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const auto error = ::GetLastError();
        return std::unexpected(std::format(
            "cannot open '{}' for durable sync: {}", path.string(), win32_message(error)));
    }
    if (::FlushFileBuffers(handle) == 0) {
        const auto error = ::GetLastError();
        ::CloseHandle(handle);
        return std::unexpected(std::format(
            "durable sync '{}' failed: {}", path.string(), win32_message(error)));
    }
    if (::CloseHandle(handle) == 0) {
        const auto error = ::GetLastError();
        return std::unexpected(std::format(
            "close synced file '{}' failed: {}", path.string(), win32_message(error)));
    }
    return {};
}

std::expected<void, std::string> sync_directory(const std::filesystem::path&) {
    return {};
}

} // namespace mcpp::platform
