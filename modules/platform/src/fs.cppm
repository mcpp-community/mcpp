// mcpp.platform.fs — platform-specific filesystem operations.
//
// Consolidates three patterns that were duplicated across the codebase:
//   1. self_exe_path()  — current executable path (was in config.cppm &
//                         ninja_backend.cppm)
//   2. which()          — find an executable in PATH (was in config.cppm &
//                         probe.cppm)
//   3. FileLock         — RAII exclusive file lock (was in bmi_cache.cppm)
//
// The first two are mcpplibs.os's; replace_file, append_atomically and the
// lock are each system's own (posix/fs.cpp, windows/fs.cpp).

export module mcpp.platform.fs;

import std;
import mcpp.platform.common;
import mcpplibs.os.env;

export namespace mcpp::platform::fs {

// ── self_exe_path ─────────────────────────────────────────────────────────
//
// Returns the absolute path of the currently running executable, as the
// system states it (mcpplibs::os::env::image).
std::filesystem::path self_exe_path();

// ── which ─────────────────────────────────────────────────────────────────
//
// Find an executable by name in PATH, with PATHEXT's suffixes on Windows
// (mcpplibs::os::env::find_executable). PATH is read as it is now: mcpp sets
// variables of its own environment for the programs it starts. A name that is
// also a shell builtin (`true`) is the program of that name on PATH.
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
// Not mcpplibs::os::fs::lock, whose POSIX lock is an open file description's
// (F_OFD_SETLK): an flock and such a lock do not exclude each other, and every
// release before this one holds these files with flock -- two releases at work
// in one MCPP_HOME would each hold the lock at once.
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
    // A descriptor, or a HANDLE; -1 is neither (INVALID_HANDLE_VALUE).
    std::intptr_t native_ = -1;
    explicit FileLock(std::intptr_t native) : native_(native) {}
};

} // namespace mcpp::platform::fs

// ─── Implementation ──────────────────────────────────────────────────────

namespace mcpp::platform::fs {

std::filesystem::path self_exe_path() {
    if (auto image = mcpplibs::os::env::image()) {
        std::error_code ec;
        auto p = std::filesystem::canonical(*image, ec);
        return ec ? *image : p;
    }
    // Fallback: hope the binary is in PATH
    return "mcpp";
}

std::optional<std::filesystem::path> which(std::string_view binary_name) {
    const char* path = std::getenv("PATH");
    return mcpplibs::os::env::find_executable(binary_name, path ? path : "");
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
    if constexpr (!is_windows) return p;
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

} // namespace mcpp::platform::fs
