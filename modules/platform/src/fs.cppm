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
// A name that the shell answers with a bare word -- a builtin such as `true` --
// is then looked for in PATH directly, so the program is found where one exists.
std::optional<std::filesystem::path> which(std::string_view binary_name);

// ── extended_length ───────────────────────────────────────────────────────
//
// The form of a path that a file operation may open whatever its length.
//
// A Win32 path that is not in the extended-length form is limited to 259
// characters, and a relative path counts the working directory against that
// limit. The engine subcommands ninja invokes (`mcpp dyndep`, `mcpp stage`,
// the `bmi-*` edges, `coff-def`, the check stamps) receive paths relative to a
// build directory, and in a host-tool sub-build those paths crossed the limit:
// `clang-scan-deps` wrote a `.ddi` that `mcpp dyndep` then could not read
// (mcpp#641, item 3).
//
//   Windows  the absolute, lexically normal path with backslashes, prefixed
//            `\\?\` (a UNC path `\\server\share\x` becomes
//            `\\?\UNC\server\share\x`); a path already in that form is
//            returned unchanged.
//   POSIX    the argument, unchanged. No limit of this kind exists.
std::filesystem::path extended_length(const std::filesystem::path& p);

// The Windows spelling rule of `extended_length`, as a pure function of an
// absolute, generic (forward-slash or backslash) path string, so that the rule
// is tested on every host. An empty or relative input is returned unchanged.
std::string windows_extended_length_spelling(std::string_view absolutePath);

// 不先删除目标文件，避免发布失败时丢失最后一份可用的编译数据库。
bool replace_file(const std::filesystem::path& source,
                  const std::filesystem::path& destination,
                  std::error_code& ec);

// Appends `bytes` to the file, creating it, in one write that another process
// appending to the same file cannot split: `O_APPEND` on POSIX, an open for
// FILE_APPEND_DATA alone on Windows, where the C runtime's append mode seeks
// and then writes, and two writers can overwrite each other.
bool append_atomically(const std::filesystem::path& file, std::string_view bytes);

// ── FileLock ──────────────────────────────────────────────────────────────
//
// RAII exclusive non-blocking file lock.
//   Windows: LockFileEx on a HANDLE
//   POSIX:   flock(2) on a file descriptor
class FileLock {
public:
    // Try to acquire an exclusive lock on <dir>/.lock (or <dir>/<name>).
    // Returns nullopt if another process holds the lock.
    static std::optional<FileLock> try_acquire(const std::filesystem::path& dir,
                                               std::string_view name = ".lock");

    // WAITS for the lock on <dir>/<name>, which `try_acquire` never does.
    //
    // Once held, `owner` is published beside the lock as <dir>/<name>.owner
    // (replaced atomically): who holds it, for a waiter to name. A sidecar
    // rather than the lock file's own bytes, because a Windows byte-range lock
    // is mandatory and would refuse that read. While the lock is taken by
    // another process, `waiting` is called once, after `notice` has passed,
    // with what the holder published (empty when it published nothing).
    //
    // The wait polls with a backoff from 20 ms to 200 ms rather than blocking
    // in the kernel: a blocking flock/LockFileEx cannot report the wait, and a
    // waiter killed by a signal holds nothing either way. nullopt only when
    // the lock file cannot be created at all (an unwritable directory, whose
    // build fails on its own terms).
    static std::optional<FileLock> acquire(
        const std::filesystem::path& dir, std::string_view name, std::string_view owner,
        std::chrono::milliseconds notice,
        const std::function<void(std::string_view holder)>& waiting);

    // What the current holder of <dir>/<name> published (see acquire).
    static std::string holder(const std::filesystem::path& dir, std::string_view name);

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

    // A NAME THAT IS ALSO A SHELL BUILTIN comes back as the bare word: `command
    // -v true` prints `true`, not `/usr/bin/true`, because the shell answers
    // with what it would run. The answer is still a name on PATH -- the program
    // exists -- so PATH is walked here rather than the name reported as missing.
    // Found while verifying a bare-name payload override (mcpp#755): the
    // refusal said `'true' is not found on PATH` on a machine carrying
    // /usr/bin/true, which is the kind of answer that sends a reader to the
    // wrong place.
    if (rc == 0 && !out.empty() && !std::filesystem::exists(out)
        && std::filesystem::path(out).filename() == out) {
        if (const char* path = std::getenv("PATH")) {
#if defined(_WIN32)
            constexpr char sep = ';';
#else
            constexpr char sep = ':';
#endif
            std::string_view rest(path);
            while (!rest.empty()) {
                const auto at = rest.find(sep);
                const auto dir = rest.substr(0, at);
                if (!dir.empty()) {
                    std::error_code ec;
                    const auto cand = std::filesystem::path(dir) / out;
                    if (std::filesystem::is_regular_file(cand, ec)) return cand;
                }
                if (at == std::string_view::npos) break;
                rest.remove_prefix(at + 1);
            }
        }
    }

    if (rc != 0 || out.empty()) return std::nullopt;
    if (!std::filesystem::exists(out)) return std::nullopt;
    return std::filesystem::path(out);
}

std::string windows_extended_length_spelling(std::string_view in) {
    std::string s(in);
    for (auto& c : s) if (c == '/') c = '\\';
    // Already extended-length (`\\?\`) or a device path (`\\.\`): the
    // prefix disables every further interpretation, so nothing is rewritten.
    if (s.starts_with("\\\\?\\") || s.starts_with("\\\\.\\")) return s;

    std::string prefix;
    std::string rest;
    if (s.starts_with("\\\\")) {
        prefix = "\\\\?\\UNC\\";
        rest = s.substr(2);
    } else if (s.size() >= 3 && std::isalpha(static_cast<unsigned char>(s[0]))
               && s[1] == ':' && s[2] == '\\') {
        prefix = "\\\\?\\";
        rest = s;
    } else {
        return std::string(in);   // relative or rootless: not this function's input
    }

    // The prefix turns off `.`/`..` processing, so the components are resolved
    // here, lexically. A `..` never climbs above the drive or the share.
    std::vector<std::string> parts;
    std::size_t keep = prefix.ends_with("UNC\\") ? 2 : 1;   // share or drive
    std::size_t start = 0;
    while (start <= rest.size()) {
        auto end = rest.find('\\', start);
        if (end == std::string::npos) end = rest.size();
        std::string comp = rest.substr(start, end - start);
        start = end + 1;
        if (comp.empty() || comp == ".") continue;
        if (comp == "..") {
            if (parts.size() > keep) parts.pop_back();
            continue;
        }
        parts.push_back(std::move(comp));
    }
    std::string out = prefix;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i) out += '\\';
        out += parts[i];
    }
    // A bare drive keeps its root separator: `\\?\C:\`, not `\\?\C:`.
    if (parts.size() == 1 && keep == 1) out += '\\';
    return out;
}

std::filesystem::path extended_length(const std::filesystem::path& p) {
#if defined(_WIN32)
    if (p.empty()) return p;
    std::error_code ec;
    auto abs = std::filesystem::absolute(p, ec);
    if (ec) return p;
    // Through UTF-8, so that a name the ANSI code page cannot spell survives:
    // the path narrowing rule (mcpp-contributing) forbids `.string()` here.
    const auto u8 = abs.generic_u8string();
    std::string narrow(reinterpret_cast<const char*>(u8.data()), u8.size());
    const auto spelled = windows_extended_length_spelling(narrow);
    return std::filesystem::path(std::u8string(
        reinterpret_cast<const char8_t*>(spelled.data()), spelled.size()));
#else
    return p;
#endif
}

bool replace_file(const std::filesystem::path& source,
                  const std::filesystem::path& destination,
                  std::error_code& ec) {
#if defined(_WIN32)
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
#else
    // 临时文件与目标文件位于同一文件系统时，rename 提供原子替换。
    std::filesystem::rename(source, destination, ec);
    return !ec;
#endif
}

bool append_atomically(const std::filesystem::path& file, std::string_view bytes) {
#if defined(_WIN32)
    HANDLE h = CreateFileW(file.wstring().c_str(), FILE_APPEND_DATA,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()),
                              &written, nullptr) && written == bytes.size();
    CloseHandle(h);
    return ok;
#else
    const int fd = ::open(file.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0644);
    if (fd < 0) return false;
    const auto n = ::write(fd, bytes.data(), bytes.size());
    ::close(fd);
    return n == static_cast<decltype(n)>(bytes.size());
#endif
}

// ── FileLock ──────────────────────────────────────────────────────────────

std::optional<FileLock> FileLock::acquire(
    const std::filesystem::path& dir, std::string_view name, std::string_view owner,
    std::chrono::milliseconds notice,
    const std::function<void(std::string_view holder)>& waiting) {
    namespace fs = std::filesystem;
    const auto start = std::chrono::steady_clock::now();
    auto pause = std::chrono::milliseconds(20);
    bool told = false;
    for (;;) {
        if (auto lock = try_acquire(dir, name)) {
            if (!owner.empty()) {
                const auto record = dir / (std::string(name) + ".owner");
                auto tmp = record;
                tmp += std::format(".{:x}", std::random_device{}());
                {
                    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
                    out << owner;
                }
                std::error_code ec;
                fs::rename(tmp, record, ec);
                if (ec) fs::remove(tmp, ec);
            }
            return lock;
        }
        // try_acquire could not even create the file: nothing to wait for.
        std::error_code ec;
        if (!fs::exists(dir / std::string(name), ec)) return std::nullopt;
        if (!told && std::chrono::steady_clock::now() - start >= notice) {
            told = true;
            if (waiting) waiting(holder(dir, name));
        }
        std::this_thread::sleep_for(pause);
        pause = (std::min)(pause * 2, std::chrono::milliseconds(200));   // parenthesised: <windows.h> defines min
    }
}

std::string FileLock::holder(const std::filesystem::path& dir, std::string_view name) {
    std::ifstream in(dir / (std::string(name) + ".owner"), std::ios::binary);
    return in ? std::string(std::istreambuf_iterator<char>(in), {}) : std::string{};
}

#if defined(_WIN32)

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
