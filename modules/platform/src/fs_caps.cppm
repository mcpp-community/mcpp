// What a filesystem does, measured where it is, not assumed from the
// platform (mcpp-community/mcpp#809; the same table as xlings'
// `platform::fs_caps`, openxlings/xlings#671). An MCPP_HOME and a project
// land on whatever the person has: ext4, APFS, NTFS -- and NFSv3, SMB, FAT, a
// FUSE mount, WSL's drvfs. NFSv3 answers renameat2(RENAME_NOREPLACE) with
// EINVAL: `mcpp new` failed there, and so would every publication that asked
// the flag. Each primitive below asks this table, once per filesystem, and
// takes the strategy that filesystem allows; none guesses from an errno.

module;

#if defined(_WIN32)
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <aclapi.h>
#  include <sddl.h>
// GetNamedSecurityInfoW and the token calls live in advapi32.
#  pragma comment(lib, "advapi32")
#elif defined(__linux__)
#  include <cerrno>
#  include <fcntl.h>
#  include <sys/stat.h>
#  include <sys/syscall.h>
#  include <sys/vfs.h>
#  include <unistd.h>
#elif defined(__APPLE__)
#  include <cerrno>
#  include <stdio.h>
#  include <sys/mount.h>
#  include <sys/param.h>
#  include <sys/stat.h>
#  include <unistd.h>
#else
#  include <cerrno>
#  include <sys/stat.h>
#endif

export module mcpp.platform.fs_caps;

import std;

export namespace mcpp::platform {

struct FsCaps {
    std::string type;                 // "ext4", "nfs", "apfs", "NTFS", ... ("" unknown)
    bool rename_no_replace { true };  // a rename that refuses an existing name, atomically
    bool rename_exchange   { true };  // two entries swapped atomically
    bool hard_links        { true };
    bool symlinks          { true };
    bool case_sensitive    { true };
    bool measured          { false }; // false: the probe could not run; the platform's native answers

    // Whether every operation mcpp asks for is the filesystem's own.
    bool native() const { return rename_no_replace && rename_exchange && hard_links && symlinks; }
};

// The filesystem holding `dir` (an existing directory), measured once per
// filesystem per process: a scratch entry in `dir`, removed after.
FsCaps fs_caps(const std::filesystem::path& dir);

// Measured now, not cached (tests, `mcpp self doctor`).
FsCaps probe_fs_caps(const std::filesystem::path& dir);

// Publish `from` (a scratch on the same filesystem) under `to`, never over an
// existing entry, by what `caps` says the filesystem does:
//   a native rename that refuses replacement;
//   else a hard link for a file and the same link again for a symbolic link
//   (each refused as atomically when the name exists), the scratch name then
//   removed;
//   else, for a directory, a rename onto a name that is not there -- the one
//   window, a name made there in between, is the caller's own under its lock.
std::expected<void, std::string> publish_no_replace(const std::filesystem::path& from,
                                                    const std::filesystem::path& to,
                                                    const FsCaps& caps);

// The same, with the caps of `to`'s directory.
std::expected<void, std::string> rename_no_replace(const std::filesystem::path& from,
                                                   const std::filesystem::path& to);

// `a` and `b` (both present, on one filesystem) trade places: atomically where
// the filesystem exchanges; else `b` is moved aside, `a` renamed onto `b`'s
// name and the old `b` put at `a` -- a reader of `b` meets the name missing
// for that moment, never a half of either.
std::expected<void, std::string> exchange_paths(const std::filesystem::path& a,
                                                const std::filesystem::path& b);

// Whether a directory another program wrote may be read and run from: it
// belongs to this user or to the system (root; on Windows also Administrators
// and SYSTEM), and when it is not this user's, this user cannot write it.
// Empty: trusted; else why not. Using another person's home is running
// what that person can replace at any time.
std::string untrusted_because(const std::filesystem::path& dir);

// `link` made a link to the directory `target`: a symbolic link; on Windows,
// where one needs a privilege a user rarely has, a junction.
bool create_directory_link(const std::filesystem::path& target, const std::filesystem::path& link);

} // namespace mcpp::platform

namespace mcpp::platform::native {

#if defined(_WIN32)
// MoveFileExW without MOVEFILE_REPLACE_EXISTING refuses an existing name on
// every Windows filesystem (NTFS, ReFS, FAT, SMB).
int rename_no_replace(const std::filesystem::path& from, const std::filesystem::path& to) {
    return ::MoveFileExW(from.wstring().c_str(), to.wstring().c_str(), MOVEFILE_WRITE_THROUGH)
        ? 0 : static_cast<int>(::GetLastError());
}
// Windows has no atomic exchange of two names.
int exchange(const std::filesystem::path&, const std::filesystem::path&) { return ERROR_NOT_SUPPORTED; }
bool unsupported(int error) { return error == ERROR_NOT_SUPPORTED || error == ERROR_INVALID_FUNCTION; }
std::error_category const& category() { return std::system_category(); }
std::uint64_t device_of(const std::filesystem::path& dir) {
    wchar_t root[MAX_PATH] {};
    DWORD serial = 0;
    if (!::GetVolumePathNameW(dir.wstring().c_str(), root, MAX_PATH)) return 0;
    if (!::GetVolumeInformationW(root, nullptr, 0, &serial, nullptr, nullptr, nullptr, 0)) return 0;
    return serial;
}
std::string owner_problem(const std::filesystem::path& dir) {
    PSID owner = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (::GetNamedSecurityInfoW(dir.wstring().c_str(), SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION,
                                &owner, nullptr, nullptr, nullptr, &sd) != ERROR_SUCCESS)
        return "its owner cannot be read";
    bool trusted = ::IsWellKnownSid(owner, WinLocalSystemSid) || ::IsWellKnownSid(owner, WinBuiltinAdministratorsSid);
    HANDLE token = nullptr;
    if (!trusted && ::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
        DWORD size = 0;
        ::GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        std::vector<unsigned char> buffer(size);
        if (::GetTokenInformation(token, TokenUser, buffer.data(), size, &size))
            trusted = ::EqualSid(owner, reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid);
        ::CloseHandle(token);
    }
    ::LocalFree(sd);
    return trusted ? std::string{} : "it belongs to another user";
}
std::string filesystem_type(const std::filesystem::path& dir) {
    wchar_t root[MAX_PATH] {}, name[MAX_PATH] {};
    if (!::GetVolumePathNameW(dir.wstring().c_str(), root, MAX_PATH)) return {};
    if (!::GetVolumeInformationW(root, nullptr, 0, nullptr, nullptr, nullptr, name, MAX_PATH)) return {};
    return std::filesystem::path(name).string();
}
#elif defined(__linux__)
int rename_no_replace(const std::filesystem::path& from, const std::filesystem::path& to) {
    constexpr unsigned kNoReplace = 1;   // renameat2 RENAME_NOREPLACE
    return ::syscall(SYS_renameat2, AT_FDCWD, from.c_str(), AT_FDCWD, to.c_str(), kNoReplace) == 0 ? 0 : errno;
}
int exchange(const std::filesystem::path& a, const std::filesystem::path& b) {
    constexpr unsigned kExchange = 2;    // renameat2 RENAME_EXCHANGE
    return ::syscall(SYS_renameat2, AT_FDCWD, a.c_str(), AT_FDCWD, b.c_str(), kExchange) == 0 ? 0 : errno;
}
bool unsupported(int error) { return error == EINVAL || error == ENOSYS || error == EOPNOTSUPP; }
std::error_category const& category() { return std::generic_category(); }
std::string owner_problem(const std::filesystem::path& dir) {
    struct stat st {};
    if (::stat(dir.c_str(), &st) != 0) return "it cannot be read";
    if (st.st_uid == ::getuid()) return {};
    if (st.st_uid != 0) return "it belongs to another user";
    if (::access(dir.c_str(), W_OK) == 0) return "it belongs to root and this user can write it";
    return {};
}

std::uint64_t device_of(const std::filesystem::path& dir) {
    struct stat st {};
    return ::stat(dir.c_str(), &st) == 0 ? static_cast<std::uint64_t>(st.st_dev) : 0;
}
std::string filesystem_type(const std::filesystem::path& dir) {
    struct statfs st {};
    if (::statfs(dir.c_str(), &st) != 0) return {};
    switch (static_cast<unsigned long>(st.f_type)) {
        case 0xEF53: return "ext4";
        case 0x58465342: return "xfs";
        case 0x9123683E: return "btrfs";
        case 0x01021994: return "tmpfs";
        case 0x794C7630: return "overlay";
        case 0x6969: return "nfs";
        case 0xFF534D42: return "cifs";
        case 0xFE534D42: return "smb2";
        case 0x4D44: return "vfat";
        case 0x2011BAB0: return "exfat";
        case 0x7366746E: return "ntfs3";
        case 0x5346544E: return "ntfs";
        case 0x65735546: return "fuse";
        case 0x01021997: return "9p";
        case 0x2FC12FC1: return "zfs";
        case 0x00C36400: return "ceph";
        default: return std::format("0x{:x}", static_cast<unsigned long>(st.f_type));
    }
}
#elif defined(__APPLE__)
int rename_no_replace(const std::filesystem::path& from, const std::filesystem::path& to) {
    return ::renamex_np(from.c_str(), to.c_str(), RENAME_EXCL) == 0 ? 0 : errno;
}
int exchange(const std::filesystem::path& a, const std::filesystem::path& b) {
    return ::renamex_np(a.c_str(), b.c_str(), RENAME_SWAP) == 0 ? 0 : errno;
}
bool unsupported(int error) { return error == EINVAL || error == ENOTSUP || error == EOPNOTSUPP || error == ENOSYS; }
std::error_category const& category() { return std::generic_category(); }
std::string owner_problem(const std::filesystem::path& dir) {
    struct stat st {};
    if (::stat(dir.c_str(), &st) != 0) return "it cannot be read";
    if (st.st_uid == ::getuid()) return {};
    if (st.st_uid != 0) return "it belongs to another user";
    if (::access(dir.c_str(), W_OK) == 0) return "it belongs to root and this user can write it";
    return {};
}

std::uint64_t device_of(const std::filesystem::path& dir) {
    struct stat st {};
    return ::stat(dir.c_str(), &st) == 0 ? static_cast<std::uint64_t>(st.st_dev) : 0;
}
std::string filesystem_type(const std::filesystem::path& dir) {
    struct statfs st {};
    return ::statfs(dir.c_str(), &st) == 0 ? std::string(st.f_fstypename) : std::string{};
}
#else
// No native primitive: the strategies below that need none.
std::string owner_problem(const std::filesystem::path&) { return {}; }
int rename_no_replace(const std::filesystem::path&, const std::filesystem::path&) { return ENOSYS; }
int exchange(const std::filesystem::path&, const std::filesystem::path&) { return ENOSYS; }
bool unsupported(int error) { return error == ENOSYS || error == EINVAL; }
std::error_category const& category() { return std::generic_category(); }
std::uint64_t device_of(const std::filesystem::path& dir) {
    struct stat st {};
    return ::stat(dir.c_str(), &st) == 0 ? static_cast<std::uint64_t>(st.st_dev) : 0;
}
std::string filesystem_type(const std::filesystem::path&) { return {}; }
#endif

} // namespace mcpp::platform::native

namespace mcpp::platform {

namespace fs = std::filesystem;

FsCaps probe_fs_caps(const fs::path& dir) {
    FsCaps caps;
    caps.type = native::filesystem_type(dir);
    std::error_code ec;
    std::random_device random;
    const auto scratch = dir / std::format(".mcpp-fsprobe-{:08x}{:08x}", random(), random());
    if (!fs::create_directory(scratch, ec) || ec) return caps;   // cannot write here: native answers
    const auto touch = [&](const fs::path& p) { std::ofstream(p).put('x'); return fs::exists(p, ec); };
    if (touch(scratch / "a") && touch(scratch / "c")) {
        caps.measured = true;
        if (const int e = native::rename_no_replace(scratch / "a", scratch / "b"); e != 0) {
            caps.rename_no_replace = !native::unsupported(e);
            fs::rename(scratch / "a", scratch / "b", ec);
        }
        if (const int e = native::exchange(scratch / "b", scratch / "c"); e != 0)
            caps.rename_exchange = !native::unsupported(e);
        ec.clear();
        fs::create_hard_link(scratch / "b", scratch / "d", ec);
        caps.hard_links = !ec;
        ec.clear();
        fs::create_symlink("b", scratch / "e", ec);
        caps.symlinks = !ec;
        ec.clear();
        if (touch(scratch / "Case")) caps.case_sensitive = !fs::exists(scratch / "cASE", ec);
    }
    fs::remove_all(scratch, ec);
    return caps;
}

FsCaps fs_caps(const fs::path& dir) {
    static std::mutex lock;
    static std::map<std::uint64_t, FsCaps> known;
    const auto device = native::device_of(dir);
    {
        std::scoped_lock guard(lock);
        if (device != 0)
            if (const auto it = known.find(device); it != known.end()) return it->second;
    }
    auto caps = probe_fs_caps(dir);
    // An unmeasured answer is not kept: the next directory of that filesystem
    // may be writable.
    if (device != 0 && caps.measured) {
        std::scoped_lock guard(lock);
        known.emplace(device, caps);
    }
    return caps;
}

std::expected<void, std::string> publish_no_replace(const fs::path& from, const fs::path& to, const FsCaps& caps) {
    const auto failed = [&](std::error_code error) {
        return std::unexpected(std::format("cannot publish {} as {}: {}", from.string(), to.string(), error.message()));
    };
    if (caps.rename_no_replace) {
        const int e = native::rename_no_replace(from, to);
        if (e == 0) return {};
        if (!native::unsupported(e)) return failed(std::error_code(e, native::category()));
        // Measured elsewhere on this filesystem, refused here (a mount under
        // the directory probed): the strategies below.
    }
    std::error_code ec;
    const auto status = fs::symlink_status(from, ec);
    if (ec) return failed(ec);
    if (status.type() == fs::file_type::symlink) {
        // A link made again with the same target refuses a name that is
        // there, as the rename flag would. (A hard link to a symbolic link is
        // the link on Linux but its target on macOS.)
        fs::copy_symlink(from, to, ec);
        if (ec) return failed(ec);
        fs::remove(from, ec);
        return {};
    }
    if (status.type() != fs::file_type::directory && caps.hard_links) {
        fs::create_hard_link(from, to, ec);
        if (ec) return failed(ec);
        fs::remove(from, ec);
        return {};
    }
    const auto there = fs::symlink_status(to, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) return failed(ec);
    if (fs::exists(there)) return failed(std::make_error_code(std::errc::file_exists));
    fs::rename(from, to, ec);
    if (ec) return failed(ec);
    return {};
}

std::expected<void, std::string> rename_no_replace(const fs::path& from, const fs::path& to) {
    auto parent = to.parent_path();
    if (parent.empty()) parent = ".";
    return publish_no_replace(from, to, fs_caps(parent));
}

bool create_directory_link(const fs::path& target, const fs::path& link) {
    std::error_code ec;
    fs::create_directory_symlink(target, link, ec);
    if (!ec) return true;
#if defined(_WIN32)
    const auto cmd = std::format("cmd /c mklink /J \"{}\" \"{}\" >nul 2>&1", link.string(), target.string());
    return std::system(cmd.c_str()) == 0;
#else
    return false;
#endif
}

std::string untrusted_because(const fs::path& dir) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return "it is not a directory";
    return native::owner_problem(dir);
}

std::expected<void, std::string> exchange_paths(const fs::path& a, const fs::path& b) {
    auto parent = b.parent_path();
    if (parent.empty()) parent = ".";
    if (fs_caps(parent).rename_exchange) {
        const int e = native::exchange(a, b);
        if (e == 0) return {};
        if (!native::unsupported(e))
            return std::unexpected(std::format("cannot exchange {} and {}: {}", a.string(), b.string(),
                                               std::error_code(e, native::category()).message()));
    }
    std::error_code ec;
    std::random_device random;
    const auto aside = parent / std::format(".{}.mcpp-aside-{:08x}", b.filename().string(), random());
    fs::rename(b, aside, ec);
    if (ec) return std::unexpected(std::format("cannot move {} aside: {}", b.string(), ec.message()));
    fs::rename(a, b, ec);
    if (ec) {
        std::error_code back;
        fs::rename(aside, b, back);
        return std::unexpected(std::format("cannot move {} to {}: {}", a.string(), b.string(), ec.message()));
    }
    fs::rename(aside, a, ec);
    if (ec) return std::unexpected(std::format("cannot move the old {} to {}: {}", b.string(), a.string(), ec.message()));
    return {};
}

} // namespace mcpp::platform
