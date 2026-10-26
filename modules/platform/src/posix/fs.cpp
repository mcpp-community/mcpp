// mcpp.platform.fs on Linux and macOS: rename's replacement, O_APPEND, and
// flock.
module;
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

module mcpp.platform.fs;

import std;

namespace mcpp::platform::fs {

bool replace_file(const std::filesystem::path& source,
                  const std::filesystem::path& destination,
                  std::error_code& ec) {
    // 临时文件与目标文件位于同一文件系统时，rename 提供原子替换。
    std::filesystem::rename(source, destination, ec);
    return !ec;
}

bool append_atomically(const std::filesystem::path& file, std::string_view bytes) {
    const int fd = ::open(file.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    const auto n = ::write(fd, bytes.data(), bytes.size());
    ::close(fd);
    return n == static_cast<decltype(n)>(bytes.size());
}

// ── FileLock ──

std::optional<FileLock> FileLock::try_acquire(const std::filesystem::path& dir,
                                              std::string_view name) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    auto lockPath = dir / std::string(name);
    int fd = ::open(lockPath.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (fd < 0) return std::nullopt;
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd);
        return std::nullopt;
    }
    return FileLock{fd};
}

FileLock::~FileLock() {
    if (native_ >= 0) {
        ::flock(static_cast<int>(native_), LOCK_UN);
        ::close(static_cast<int>(native_));
    }
}

FileLock::FileLock(FileLock&& o) noexcept : native_(o.native_) {
    o.native_ = -1;
}

FileLock& FileLock::operator=(FileLock&& o) noexcept {
    if (this != &o) {
        if (native_ >= 0) {
            ::flock(static_cast<int>(native_), LOCK_UN);
            ::close(static_cast<int>(native_));
        }
        native_ = o.native_;
        o.native_ = -1;
    }
    return *this;
}

} // namespace mcpp::platform::fs
