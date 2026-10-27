// mcpp.build.dirlock — the lock a configuration's build directory is written
// under (2026-10-26 design §4, mcpp#839).
//
// ONE LOCK PER BUILD DIRECTORY, HELD ONLY WHILE IT IS WRITTEN.
//
// Two builds of one configuration used to write one directory at once: each
// planned, each wrote build.ninja, and each ran ninja over the other's half
// written module files. Measured on a 24-module project: three rounds of five
// failed on both sides (`failed to write/read compiled module`). Every other
// shared, mutable thing a build touches was already protected (the index
// refresh, each BMI cache entry, each toolchain version directory); the build
// directory was the one that was not.
//
// The lock is taken where the directory starts being written -- the finish
// phase of `prepare_build`, once the fingerprint names the directory -- and is
// held through ninja. Planning before that (the manifest, resolution, the
// network, the toolchain, the scan) holds nothing, a different configuration
// has a different directory, and a query never waits. `mcpp run` releases it
// before the program starts (the descriptor is close-on-exec); `mcpp test`
// releases it before the first test runs. A crashed holder holds nothing: the
// operating system releases the lock with the process.
//
// A waiter is told who holds it once the wait has lasted 300 ms, which is what
// the owner record exists for: the process, the command, the caller's label
// (`MCPP_CLIENT`), and how long ago it started.
export module mcpp.build.dirlock;

import std;
import mcpp.invocation;
import mcpp.home.files;
import mcpp.ui;

export namespace mcpp::build::dirlock {

inline constexpr std::string_view kLockName = ".mcpp-build.lock";

// The holder a waiter is shown, parsed from the owner record. Fields absent
// from the record are empty.
struct Holder {
    std::string pid;
    std::string command;
    std::string client;
    std::int64_t startedAt = 0;   // seconds since the epoch, 0 when unknown
};

Holder parse_holder(std::string_view record);

// "pid 1234 (mcpp build, from mcpp-ide, 8s)" -- or "another mcpp" when the
// holder published nothing.
std::string describe(const Holder& h, std::int64_t now);

// This process's owner record.
std::string owner_record();

// This process, as the owner record and the result record name it.
long process_id();
const std::string& command();

// The command this process runs, as the owner record names it: set once from
// argv by the CLI, "mcpp" otherwise.
void set_command(std::string command);

// Called once when a wait has lasted long enough to report (the event stream
// listens here; the human line is printed by `acquire` itself).
void set_waiting_observer(std::function<void(const Holder&, std::chrono::milliseconds)> observer);

// Waits for the build directory's lock. Shared, because a BuildContext is
// copied, and the last copy to go releases it; `release` drops it early.
std::shared_ptr<mcpp::home::FileLock> acquire(const std::filesystem::path& outputDir);

}  // namespace mcpp::build::dirlock

namespace mcpp::build::dirlock {

namespace {
std::string& command_slot() {
    static std::string command = "mcpp";
    return command;
}
std::function<void(const Holder&, std::chrono::milliseconds)>& observer_slot() {
    static std::function<void(const Holder&, std::chrono::milliseconds)> observer;
    return observer;
}
long current_pid() { return mcpp::invocation::process_id(); }
std::int64_t now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
}  // namespace

void set_command(std::string command) { command_slot() = std::move(command); }
const std::string& command() { return command_slot(); }
long process_id() { return current_pid(); }

void set_waiting_observer(std::function<void(const Holder&, std::chrono::milliseconds)> observer) {
    observer_slot() = std::move(observer);
}

Holder parse_holder(std::string_view record) {
    Holder h;
    std::size_t pos = 0;
    while (pos < record.size()) {
        auto end = record.find('\n', pos);
        if (end == std::string_view::npos) end = record.size();
        const auto line = record.substr(pos, end - pos);
        pos = end + 1;
        const auto eq = line.find('=');
        if (eq == std::string_view::npos) continue;
        const auto key = line.substr(0, eq);
        const auto value = std::string(line.substr(eq + 1));
        if (key == "pid") h.pid = value;
        else if (key == "command") h.command = value;
        else if (key == "client") h.client = value;
        else if (key == "started") {
            std::int64_t v = 0;
            const auto r = std::from_chars(value.data(), value.data() + value.size(), v);
            if (r.ec == std::errc{}) h.startedAt = v;
        }
    }
    return h;
}

std::string describe(const Holder& h, std::int64_t now) {
    if (h.pid.empty()) return "another mcpp";
    std::vector<std::string> parts;
    if (!h.command.empty()) parts.push_back(h.command);
    if (!h.client.empty()) parts.push_back("from " + h.client);
    if (h.startedAt > 0 && now >= h.startedAt) parts.push_back(std::format("{}s", now - h.startedAt));
    std::string out = "pid " + h.pid;
    if (!parts.empty()) {
        out += " (";
        for (std::size_t i = 0; i < parts.size(); ++i) out += (i ? ", " : "") + parts[i];
        out += ")";
    }
    return out;
}

std::string owner_record() {
    std::string client = mcpp::invocation::get("MCPP_CLIENT").value_or("");
    return std::format("pid={}\nstarted={}\ncommand={}\nclient={}\n",
                       current_pid(), now_seconds(),
                       command_slot(), client);
}

// HELD ONCE PER PROCESS. One run may enter a configuration's directory more
// than once -- `mcpp run --format <f>` builds the program and then packs it,
// and the pack plans the same configuration -- and a second lock on the same
// file from the same process waits for itself (flock and LockFileEx lock per
// open file, not per process). The lock is shared while any holder keeps it.
std::mutex g_heldMutex;
std::map<std::string, std::weak_ptr<mcpp::home::FileLock>>& held() {
    static std::map<std::string, std::weak_ptr<mcpp::home::FileLock>> m;
    return m;
}

std::shared_ptr<mcpp::home::FileLock> acquire(const std::filesystem::path& outputDir) {
    const auto key = outputDir.lexically_normal().generic_string();
    std::lock_guard heldLock(g_heldMutex);
    if (auto existing = held()[key].lock()) return existing;
    const auto start = std::chrono::steady_clock::now();
    auto lock = mcpp::home::FileLock::acquire(
        outputDir, kLockName, owner_record(), std::chrono::milliseconds(300),
        [&](std::string_view record) {
            const auto holder = parse_holder(record);
            mcpp::ui::status("Waiting", std::format("for the build directory, held by {}",
                                                     describe(holder, now_seconds())));
            if (auto& observe = observer_slot())
                observe(holder, std::chrono::duration_cast<std::chrono::milliseconds>(
                                    std::chrono::steady_clock::now() - start));
        });
    if (!lock) return nullptr;
    auto shared = std::make_shared<mcpp::home::FileLock>(std::move(*lock));
    held()[key] = shared;
    return shared;
}

}  // namespace mcpp::build::dirlock
