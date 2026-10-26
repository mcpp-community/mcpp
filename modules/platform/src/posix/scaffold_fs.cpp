// mcpp.platform.scaffold_fs on Linux and macOS: fsync of a file and of a
// directory.
module;
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

module mcpp.platform.scaffold_fs;

import std;

namespace mcpp::platform {

namespace {

std::string errno_message(int code) {
    return std::generic_category().message(code);
}

bool sync_is_unsupported(int code) {
    return code == EINVAL || code == EROFS || code == ENOTSUP || code == EOPNOTSUPP;
}

} // namespace

std::expected<void, std::string> sync_regular_file(const std::filesystem::path& path) {
    int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        const int error = errno;
        return std::unexpected(std::format(
            "cannot open '{}' for durable sync: {}", path.string(), errno_message(error)));
    }
    if (::fsync(descriptor) != 0) {
        const int error = errno;
        ::close(descriptor);
        if (sync_is_unsupported(error)) return {};
        return std::unexpected(std::format(
            "durable sync '{}' failed: {}", path.string(), errno_message(error)));
    }
    if (::close(descriptor) != 0) {
        const int error = errno;
        return std::unexpected(std::format(
            "close synced file '{}' failed: {}", path.string(), errno_message(error)));
    }
    return {};
}

std::expected<void, std::string> sync_directory(const std::filesystem::path& path) {
    int descriptor = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0) {
        const int error = errno;
        if (sync_is_unsupported(error)) return {};
        return std::unexpected(std::format(
            "cannot open directory '{}' for durable sync: {}", path.string(), errno_message(error)));
    }
    if (::fsync(descriptor) != 0) {
        const int error = errno;
        ::close(descriptor);
        if (sync_is_unsupported(error)) return {};
        return std::unexpected(std::format(
            "durable directory sync '{}' failed: {}", path.string(), errno_message(error)));
    }
    if (::close(descriptor) != 0) {
        const int error = errno;
        return std::unexpected(std::format(
            "close synced directory '{}' failed: {}", path.string(), errno_message(error)));
    }
    return {};
}

} // namespace mcpp::platform
