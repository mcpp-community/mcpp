// Platform filesystem primitives used by transactional scaffolding.  The
// public contract is stronger than std::filesystem::rename: an existing final
// name is never replaced, including a target that appears in the commit race.

module;

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#elif defined(__linux__)
#  include <cerrno>
#  include <fcntl.h>
#  include <linux/fs.h>
#  include <sys/syscall.h>
#  include <unistd.h>
#elif defined(__APPLE__)
#  include <cerrno>
#  include <fcntl.h>
#  include <stdio.h>
#  include <unistd.h>
#else
#  include <cerrno>
#  include <fcntl.h>
#  include <unistd.h>
#endif

export module mcpp.platform.scaffold_fs;

import std;

namespace mcpp::platform::detail {

#if defined(_WIN32)
std::string win32_message(DWORD code) {
    return std::system_category().message(static_cast<int>(code));
}
#else
std::string errno_message(int code) {
    return std::generic_category().message(code);
}

bool sync_is_unsupported(int code) {
    return code == EINVAL || code == EROFS
#ifdef ENOTSUP
        || code == ENOTSUP
#endif
#ifdef EOPNOTSUPP
        || code == EOPNOTSUPP
#endif
        ;
}
#endif

} // namespace mcpp::platform::detail

export namespace mcpp::platform {

std::expected<void, std::string> atomic_rename_directory_no_replace(
    const std::filesystem::path& source,
    const std::filesystem::path& target) {
#if defined(_WIN32)
    if (::MoveFileExW(source.c_str(), target.c_str(),
                      MOVEFILE_WRITE_THROUGH) != 0) {
        return {};
    }
    const auto error = ::GetLastError();
    return std::unexpected(std::format(
        "atomic no-replace rename '{}' -> '{}' failed: {}",
        source.string(), target.string(), detail::win32_message(error)));
#elif defined(__linux__) && defined(SYS_renameat2)
    if (::syscall(SYS_renameat2, AT_FDCWD, source.c_str(), AT_FDCWD,
                  target.c_str(), RENAME_NOREPLACE) == 0) {
        return {};
    }
    const int error = errno;
    return std::unexpected(std::format(
        "atomic no-replace rename '{}' -> '{}' failed: {}",
        source.string(), target.string(), detail::errno_message(error)));
#elif defined(__APPLE__)
    if (::renamex_np(source.c_str(), target.c_str(), RENAME_EXCL) == 0) {
        return {};
    }
    const int error = errno;
    return std::unexpected(std::format(
        "atomic no-replace rename '{}' -> '{}' failed: {}",
        source.string(), target.string(), detail::errno_message(error)));
#else
    // The fallback remains no-clobber in ordinary use, but platforms without
    // a native exclusive rename cannot close the existence-check race.  All
    // release platforms use one of the branches above.
    std::error_code ec;
    if (std::filesystem::exists(target, ec) || ec) {
        return std::unexpected(std::format(
            "atomic scaffold target '{}' already exists", target.string()));
    }
    std::filesystem::rename(source, target, ec);
    if (!ec) return {};
    return std::unexpected(std::format(
        "scaffold rename '{}' -> '{}' failed: {}",
        source.string(), target.string(), ec.message()));
#endif
}

// Make a newly written regular file durable before its containing staging
// directory is published. Unsupported filesystem sync operations are treated
// as "best available"; genuine I/O/permission failures abort the transaction.
std::expected<void, std::string> sync_regular_file(
    const std::filesystem::path& path) {
#if defined(_WIN32)
    HANDLE handle = ::CreateFileW(
        path.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        const auto error = ::GetLastError();
        return std::unexpected(std::format(
            "cannot open '{}' for durable sync: {}", path.string(),
            detail::win32_message(error)));
    }
    if (::FlushFileBuffers(handle) == 0) {
        const auto error = ::GetLastError();
        ::CloseHandle(handle);
        return std::unexpected(std::format(
            "durable sync '{}' failed: {}", path.string(),
            detail::win32_message(error)));
    }
    if (::CloseHandle(handle) == 0) {
        const auto error = ::GetLastError();
        return std::unexpected(std::format(
            "close synced file '{}' failed: {}", path.string(),
            detail::win32_message(error)));
    }
    return {};
#else
    int descriptor = ::open(path.c_str(), O_RDONLY
#ifdef O_CLOEXEC
                            | O_CLOEXEC
#endif
    );
    if (descriptor < 0) {
        const int error = errno;
        return std::unexpected(std::format(
            "cannot open '{}' for durable sync: {}", path.string(),
            detail::errno_message(error)));
    }
    if (::fsync(descriptor) != 0) {
        const int error = errno;
        ::close(descriptor);
        if (detail::sync_is_unsupported(error)) return {};
        return std::unexpected(std::format(
            "durable sync '{}' failed: {}", path.string(),
            detail::errno_message(error)));
    }
    if (::close(descriptor) != 0) {
        const int error = errno;
        return std::unexpected(std::format(
            "close synced file '{}' failed: {}", path.string(),
            detail::errno_message(error)));
    }
    return {};
#endif
}

// Persist the directory entry after the exclusive rename where the host/filesystem
// offers directory fsync. MoveFileExW(MOVEFILE_WRITE_THROUGH) already supplies
// the corresponding Windows guarantee.
std::expected<void, std::string> sync_directory(
    const std::filesystem::path& path) {
#if defined(_WIN32)
    (void)path;
    return {};
#else
    int descriptor = ::open(path.c_str(), O_RDONLY
#ifdef O_DIRECTORY
                            | O_DIRECTORY
#endif
#ifdef O_CLOEXEC
                            | O_CLOEXEC
#endif
    );
    if (descriptor < 0) {
        const int error = errno;
        if (detail::sync_is_unsupported(error)) return {};
        return std::unexpected(std::format(
            "cannot open directory '{}' for durable sync: {}", path.string(),
            detail::errno_message(error)));
    }
    if (::fsync(descriptor) != 0) {
        const int error = errno;
        ::close(descriptor);
        if (detail::sync_is_unsupported(error)) return {};
        return std::unexpected(std::format(
            "durable directory sync '{}' failed: {}", path.string(),
            detail::errno_message(error)));
    }
    if (::close(descriptor) != 0) {
        const int error = errno;
        return std::unexpected(std::format(
            "close synced directory '{}' failed: {}", path.string(),
            detail::errno_message(error)));
    }
    return {};
#endif
}

} // namespace mcpp::platform
