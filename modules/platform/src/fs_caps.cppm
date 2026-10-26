// What a filesystem does, measured where it is, not assumed from the
// platform (mcpp-community/mcpp#809; the same table as xlings'
// `platform::fs_caps`, openxlings/xlings#671). An MCPP_HOME and a project
// land on whatever the person has: ext4, APFS, NTFS -- and NFSv3, SMB, FAT, a
// FUSE mount, WSL's drvfs. NFSv3 answers renameat2(RENAME_NOREPLACE) with
// EINVAL: `mcpp new` failed there, and so would every publication that asked
// the flag. Each primitive below asks this table, once per filesystem, and
// takes the strategy that filesystem allows; none guesses from an errno.

export module mcpp.platform.fs_caps;

import std;
import mcpplibs.os.fs;

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

// Each system's own, in linux/, macos/ and windows/ fs_caps.cpp: what
// mcpplibs.os does not carry, because no other system has it.

// `a` and `b` trade places, atomically: 0, else the system's code, which
// `unsupported` says is the filesystem declining.
int exchange(const std::filesystem::path& a, const std::filesystem::path& b);
bool unsupported(int error);
std::error_category const& category();

// Empty when `dir`'s owner is this user or the system; else why not.
std::string owner_problem(const std::filesystem::path& dir);

// The filesystem's name, as the system reports it ("" unknown).
std::string filesystem_type(const std::filesystem::path& dir);

} // namespace mcpp::platform::native

namespace mcpp::platform {

// Not `fs`: mcpp::platform::fs is the namespace of mcpp.platform.fs, and an
// alias of the same name beside it is ill-formed (GCC 16 crashes reading the
// two modules into one translation unit).
namespace stdfs = std::filesystem;

FsCaps probe_fs_caps(const stdfs::path& dir) {
    FsCaps caps;
    caps.type = native::filesystem_type(dir);
    std::error_code ec;
    std::random_device random;
    const auto scratch = dir / std::format(".mcpp-fsprobe-{:08x}{:08x}", random(), random());
    if (!stdfs::create_directory(scratch, ec) || ec) return caps;   // cannot write here: native answers
    const auto touch = [&](const stdfs::path& p) { std::ofstream(p).put('x'); return stdfs::exists(p, ec); };
    if (touch(scratch / "a") && touch(scratch / "c")) {
        caps.measured = true;
        if (const auto r = mcpplibs::os::fs::rename(scratch / "a", scratch / "b", false); !r) {
            caps.rename_no_replace = r.error().code != mcpplibs::os::errc::not_supported;
            stdfs::rename(scratch / "a", scratch / "b", ec);
        }
        if (const int e = native::exchange(scratch / "b", scratch / "c"); e != 0)
            caps.rename_exchange = !native::unsupported(e);
        ec.clear();
        stdfs::create_hard_link(scratch / "b", scratch / "d", ec);
        caps.hard_links = !ec;
        ec.clear();
        stdfs::create_symlink("b", scratch / "e", ec);
        caps.symlinks = !ec;
        ec.clear();
        if (touch(scratch / "Case")) caps.case_sensitive = !stdfs::exists(scratch / "cASE", ec);
    }
    stdfs::remove_all(scratch, ec);
    return caps;
}

FsCaps fs_caps(const stdfs::path& dir) {
    static std::mutex lock;
    static std::map<std::uint64_t, FsCaps> known;
    const auto identity = mcpplibs::os::fs::identity_of(dir);
    const std::uint64_t device = identity ? identity->volume : 0;
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

std::expected<void, std::string> publish_no_replace(const stdfs::path& from, const stdfs::path& to, const FsCaps& caps) {
    const auto failed = [&](std::error_code error) {
        return std::unexpected(std::format("cannot publish {} as {}: {}", from.string(), to.string(), error.message()));
    };
    if (caps.rename_no_replace) {
        const auto r = mcpplibs::os::fs::rename(from, to, false);
        if (r) return {};
        if (r.error().code != mcpplibs::os::errc::not_supported)
            return std::unexpected(std::format("cannot publish {} as {}: {}", from.string(), to.string(),
                                               mcpplibs::os::describe(r.error().code)));
        // Measured elsewhere on this filesystem, refused here (a mount under
        // the directory probed): the strategies below.
    }
    std::error_code ec;
    const auto status = stdfs::symlink_status(from, ec);
    if (ec) return failed(ec);
    if (status.type() == stdfs::file_type::symlink) {
        // A link made again with the same target refuses a name that is
        // there, as the rename flag would. (A hard link to a symbolic link is
        // the link on Linux but its target on macOS.)
        stdfs::copy_symlink(from, to, ec);
        if (ec) return failed(ec);
        stdfs::remove(from, ec);
        return {};
    }
    if (status.type() != stdfs::file_type::directory && caps.hard_links) {
        stdfs::create_hard_link(from, to, ec);
        if (ec) return failed(ec);
        stdfs::remove(from, ec);
        return {};
    }
    const auto there = stdfs::symlink_status(to, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) return failed(ec);
    if (stdfs::exists(there)) return failed(std::make_error_code(std::errc::file_exists));
    stdfs::rename(from, to, ec);
    if (ec) return failed(ec);
    return {};
}

std::expected<void, std::string> rename_no_replace(const stdfs::path& from, const stdfs::path& to) {
    auto parent = to.parent_path();
    if (parent.empty()) parent = ".";
    return publish_no_replace(from, to, fs_caps(parent));
}

bool create_directory_link(const stdfs::path& target, const stdfs::path& link) {
    return mcpplibs::os::fs::create_link(target, link, true).has_value();
}

std::string untrusted_because(const stdfs::path& dir) {
    std::error_code ec;
    if (!stdfs::is_directory(dir, ec)) return "it is not a directory";
    return native::owner_problem(dir);
}

std::expected<void, std::string> exchange_paths(const stdfs::path& a, const stdfs::path& b) {
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
    stdfs::rename(b, aside, ec);
    if (ec) return std::unexpected(std::format("cannot move {} aside: {}", b.string(), ec.message()));
    stdfs::rename(a, b, ec);
    if (ec) {
        std::error_code back;
        stdfs::rename(aside, b, back);
        return std::unexpected(std::format("cannot move {} to {}: {}", a.string(), b.string(), ec.message()));
    }
    stdfs::rename(aside, a, ec);
    if (ec) return std::unexpected(std::format("cannot move the old {} to {}: {}", b.string(), a.string(), ec.message()));
    return {};
}

} // namespace mcpp::platform
