// mcpp.platform.fs — platform-specific filesystem operations.
//
// Consolidates three patterns that were duplicated across the codebase:
//   1. self_exe_path()  — current executable path (was in config.cppm &
//                         ninja_backend.cppm)
//   2. which()          — find an executable in PATH (was in config.cppm &
//                         probe.cppm)
//   3. FileLock         — RAII exclusive file lock (was in bmi_cache.cppm)

module;
#include <cstdio>
#include <cstdlib>
#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#define popen  _popen
#define pclose _pclose
#elif defined(__APPLE__)
#include <mach-o/dyld.h>  // _NSGetExecutablePath
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

// POSIX file locking (shared between macOS and Linux)
#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

export module mcpp.platform.fs;

import std;
import mcpp.platform.common;

export namespace mcpp::platform::fs {

// ── self_exe_path ─────────────────────────────────────────────────────────
//
// Returns the absolute path of the currently running executable.
//   Windows: GetModuleFileNameA
//   macOS:   _NSGetExecutablePath
//   Linux:   /proc/self/exe
std::filesystem::path self_exe_path();

// ── which ─────────────────────────────────────────────────────────────────
//
// Find an executable by name in PATH.
//   Windows: `where <name>`
//   POSIX:   `command -v <name>`
std::optional<std::filesystem::path> which(std::string_view binary_name);

// ── FileLock ──────────────────────────────────────────────────────────────
//
// RAII exclusive non-blocking file lock.
//   Windows: LockFileEx on a HANDLE
//   POSIX:   flock(2) on a file descriptor
class FileLock {
public:
    // Try to acquire an exclusive lock on <dir>/.lock.
    // Returns nullopt if another process holds the lock.
    static std::optional<FileLock> try_acquire(const std::filesystem::path& dir);

    ~FileLock();
    FileLock(FileLock&& o) noexcept;
    FileLock& operator=(FileLock&& o) noexcept;

    FileLock(const FileLock&) = delete;
    FileLock& operator=(const FileLock&) = delete;

private:
#if defined(_WIN32)
    void* handle_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(-1)); // INVALID_HANDLE_VALUE
    explicit FileLock(void* h) : handle_(h) {}
#else
    int fd_ = -1;
    explicit FileLock(int fd) : fd_(fd) {}
#endif
};

} // namespace mcpp::platform::fs

// ─── Implementation ──────────────────────────────────────────────────────

namespace mcpp::platform::fs {

std::filesystem::path self_exe_path() {
    std::error_code ec;
#if defined(_WIN32)
    char buf[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        auto p = std::filesystem::canonical(buf, ec);
        if (!ec) return p;
    }
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) {
        auto p = std::filesystem::canonical(buf, ec);
        if (!ec) return p;
    }
#else
    auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec) return p;
#endif
    // Fallback: hope the binary is in PATH
    return "mcpp";
}

std::optional<std::filesystem::path> which(std::string_view binary_name) {
    std::string name(binary_name);
#if defined(_WIN32)
    std::string cmd = "where " + name + " 2>nul";
#else
    std::string cmd = "command -v '" + name + "' 2>/dev/null </dev/null";
#endif
    std::array<char, 4096> buf{};
    std::string out;
    std::FILE* fp = ::popen(cmd.c_str(), "r");
    if (!fp) return std::nullopt;
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), fp) != nullptr)
        out += buf.data();
    int rc = ::pclose(fp);

    // Trim and take first line
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
        out.pop_back();
    auto nl = out.find('\n');
    if (nl != std::string::npos) out.resize(nl);

    if (rc != 0 || out.empty()) return std::nullopt;
    if (!std::filesystem::exists(out)) return std::nullopt;
    return std::filesystem::path(out);
}

// ── FileLock ──────────────────────────────────────────────────────────────

#if defined(_WIN32)

std::optional<FileLock> FileLock::try_acquire(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    auto lockPath = dir / ".lock";
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
    return FileLock{h};
}

FileLock::~FileLock() {
    if (handle_ != INVALID_HANDLE_VALUE) {
        OVERLAPPED ov = {};
        UnlockFileEx(static_cast<HANDLE>(handle_), 0, 1, 0, &ov);
        CloseHandle(static_cast<HANDLE>(handle_));
    }
}

FileLock::FileLock(FileLock&& o) noexcept : handle_(o.handle_) {
    o.handle_ = INVALID_HANDLE_VALUE;
}

FileLock& FileLock::operator=(FileLock&& o) noexcept {
    if (this != &o) {
        if (handle_ != INVALID_HANDLE_VALUE) {
            OVERLAPPED ov = {};
            UnlockFileEx(static_cast<HANDLE>(handle_), 0, 1, 0, &ov);
            CloseHandle(static_cast<HANDLE>(handle_));
        }
        handle_ = o.handle_;
        o.handle_ = INVALID_HANDLE_VALUE;
    }
    return *this;
}

#else // POSIX

std::optional<FileLock> FileLock::try_acquire(const std::filesystem::path& dir) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    auto lockPath = dir / ".lock";
    int fd = ::open(lockPath.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
    if (fd < 0) return std::nullopt;
    if (::flock(fd, LOCK_EX | LOCK_NB) != 0) {
        ::close(fd);
        return std::nullopt;
    }
    return FileLock{fd};
}

FileLock::~FileLock() {
    if (fd_ >= 0) {
        ::flock(fd_, LOCK_UN);
        ::close(fd_);
    }
}

FileLock::FileLock(FileLock&& o) noexcept : fd_(o.fd_) {
    o.fd_ = -1;
}

FileLock& FileLock::operator=(FileLock&& o) noexcept {
    if (this != &o) {
        if (fd_ >= 0) {
            ::flock(fd_, LOCK_UN);
            ::close(fd_);
        }
        fd_ = o.fd_;
        o.fd_ = -1;
    }
    return *this;
}

#endif

} // namespace mcpp::platform::fs
