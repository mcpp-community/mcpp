// mcpp.home.files — the files mcpp keeps under a home and a project that other
// processes share: a lock compatible with every release of mcpp, a file
// replaced without losing the last good copy, and whether a directory another
// person controls may be trusted.
export module mcpp.home.files;

import std;

export namespace mcpp::home {

// ── FileLock ──────────────────────────────────────────────────────────────
//
// RAII exclusive non-blocking file lock.
//   Windows: LockFileEx on a HANDLE
//   POSIX:   flock(2) on a file descriptor
// NOT mcpplibs::os::fs::lock, whose POSIX lock is an open file description's
// (F_OFD_SETLK): an flock and such a lock do not exclude each other, and every
// release of mcpp holds these files with flock -- two releases at work in one
// MCPP_HOME would each hold the lock at once. The lock is therefore mcpp's
// contract with its own earlier releases, and its one primitive per system is
// here (native/<system>/lock.cpp) and nowhere else in mcpp.
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

// `source` renamed over `destination`, which is not removed first: a failed
// replacement leaves the last good copy (a compilation database an editor
// reads). A destination another program holds open is retried briefly.
bool replace_file(const std::filesystem::path& source, const std::filesystem::path& destination,
                  std::error_code& ec);

// Whether a directory another program wrote may be read and run from: it
// belongs to this user or to the system (root; on Windows also Administrators
// and SYSTEM), and when it is not this user's, this user cannot write it.
// Empty: trusted; else why not. Using another person's home is running what
// that person can replace at any time.
std::string untrusted_because(const std::filesystem::path& dir);

}  // namespace mcpp::home

// The one primitive per system (native/<system>/lock.cpp). Not exported.
namespace mcpp::home::native {
// An exclusive lock on the file, taken without waiting: the handle, or -1.
std::intptr_t lock_file(const std::filesystem::path& file);
void unlock_file(std::intptr_t handle);
}  // namespace mcpp::home::native
