// mcpp.home.files: the waiting, the owner record and the trust rule over
// mcpplibs.os; the lock itself is each system's (src/native/<system>).
module mcpp.home.files;

import std;
import mcpplibs.os.fs;

namespace mcpp::home {

std::optional<FileLock> FileLock::try_acquire(const std::filesystem::path& dir, std::string_view name) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const auto handle = native::lock_file(dir / std::string(name));
    if (handle == -1) return std::nullopt;
    return FileLock{handle};
}

FileLock::~FileLock() { if (native_ != -1) native::unlock_file(native_); }

FileLock::FileLock(FileLock&& o) noexcept : native_(o.native_) { o.native_ = -1; }

FileLock& FileLock::operator=(FileLock&& o) noexcept {
    if (this != &o) {
        if (native_ != -1) native::unlock_file(native_);
        native_ = o.native_;
        o.native_ = -1;
    }
    return *this;
}

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


bool replace_file(const std::filesystem::path& source, const std::filesystem::path& destination,
                  std::error_code& ec) {
    // Windows refuses the rename while another program holds the destination
    // open (a sharing violation): an editor, a virus scanner, or another mcpp
    // reading config.toml, which holds it for the length of one read. Tries
    // with a pause that starts short and grows to 20 ms, for two seconds, then
    // the failure. A longer pause lost to readers that hold the file most of
    // the time: programs started at once on a loaded Windows runner each read
    // config.toml, and a writer that tried a dozen times in two seconds found
    // it held at each (unit Config/ConfigValueIsReplacedWhole, 2026-11-14).
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    auto delay = std::chrono::milliseconds{1};
    for (;;) {
        const auto r = mcpplibs::os::fs::rename(source, destination, true);
        if (r) { ec.clear(); return true; }
        // A refusal is retried as the sharing violation it is on Windows;
        // elsewhere it is final, and costs the failing write the wait.
        const bool held = r.error().code == mcpplibs::os::errc::again
                       || r.error().code == mcpplibs::os::errc::permission;
        if (!held || std::chrono::steady_clock::now() + delay > deadline) {
            ec = std::make_error_code(r.error().code == mcpplibs::os::errc::not_found
                                          ? std::errc::no_such_file_or_directory
                                          : std::errc::permission_denied);
            return false;
        }
        std::this_thread::sleep_for(delay);
        delay = std::min(delay * 2, std::chrono::milliseconds{20});
    }
}

bool write_replacing(const std::filesystem::path& path, std::string_view text) {
    std::error_code ec;
    // A link (a config.toml a dotfile manager keeps elsewhere) is written
    // through: the file it names is replaced, and the link stays.
    auto file = path;
    if (std::filesystem::is_symlink(path, ec))
        if (auto target = std::filesystem::canonical(path, ec); !ec) file = std::move(target);
    std::filesystem::create_directories(file.parent_path(), ec);
    auto tmp = file;
    tmp += std::format(".{:x}.tmp", std::random_device{}());
    {
        std::ofstream os(tmp);
        if (!os) return false;
        os << text;
        if (!os.flush()) {
            os.close();
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }
    // The replaced file's permissions carry over (a config.toml kept private).
    if (const auto st = std::filesystem::status(file, ec); !ec && std::filesystem::exists(st))
        std::filesystem::permissions(tmp, st.permissions(), std::filesystem::perm_options::replace, ec);
    if (!replace_file(tmp, file, ec)) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

std::optional<std::string> read_whole(const std::filesystem::path& file) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{2};
    auto delay = std::chrono::milliseconds{5};
    for (;;) {
        if (std::ifstream in{file}) {
            std::ostringstream text;
            text << in.rdbuf();
            return text.str();
        }
        std::error_code ec;
        if (!std::filesystem::exists(file, ec)
            || std::chrono::steady_clock::now() + delay > deadline)
            return std::nullopt;
        std::this_thread::sleep_for(delay);
        delay = std::min(delay * 2, std::chrono::milliseconds{200});
    }
}

std::string untrusted_because(const std::filesystem::path& dir) {
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return "it is not a directory";
    switch (mcpplibs::os::fs::owner_of(dir)) {
        case mcpplibs::os::fs::owner::this_user:    return {};
        case mcpplibs::os::fs::owner::another_user: return "it belongs to another user";
        case mcpplibs::os::fs::owner::unknown:      return "its owner cannot be read";
        case mcpplibs::os::fs::owner::system:
            return mcpplibs::os::fs::writable(dir) ? "it belongs to the system and this user can write it"
                                                   : std::string{};
    }
    return {};
}

void mark_used(const std::filesystem::path& recordFile) {
    namespace fs = std::filesystem;
    std::error_code ec;
    const auto now = fs::file_time_type::clock::now();
    const auto was = fs::last_write_time(recordFile, ec);
    if (ec || now - was < std::chrono::hours(12)) return;
    fs::last_write_time(recordFile, now, ec);
}

}  // namespace mcpp::home
