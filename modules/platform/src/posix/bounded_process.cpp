// mcpp.platform.unix.bounded_process on Linux and macOS.
module;
#include <unistd.h>      // pipe, close, read
#include <sys/wait.h>    // waitpid
#include <spawn.h>       // posix_spawnp, posix_spawn_file_actions_*
#include <signal.h>      // kill, SIGKILL
#include <cerrno>        // errno, EINTR
#include <fcntl.h>       // fcntl O_NONBLOCK
#include <time.h>        // nanosleep
#include <cstdio>        // fputs, stderr — the out-of-slots diagnostic
#include <termios.h>     // tcgetattr, tcsetattr — the terminal mode a signal restores

module mcpp.platform.unix.bounded_process;

import std;
import mcpp.platform.common;
import mcpp.platform.env;

namespace mcpp::platform::unixproc {

namespace {

char** current_environ() {
    return mcpp::platform::env::environment();
}

int normalize_status(int status) {
    if (WIFEXITED(status))   return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return status;
}

} // namespace

DeadlineRun capture_with_deadline(const char* const* argvEntries,
                                  unsigned long      argvCount,
                                  const char* const* envEntries,
                                  unsigned long      envCount,
                                  const char*        cwd,
                                  long long          deadlineMs,
                                  long long          idleMs,
                                  int                ownGroup,
                                  OutputSink         sink,
                                  void*              ctx,
                                  StopPoll           stop)
{
    DeadlineRun out;
    const bool idleBound = idleMs > 0 && sink != nullptr;
    // A child with no bound at all is accepted only with a sink: that caller
    // reads the output as it arrives (a build's progress) and has no untimed
    // path that could deliver it.
    if ((deadlineMs <= 0 && !idleBound && sink == nullptr) || argvCount == 0 || !argvEntries)
        return out;

    // The child's environment: ours, minus anything overridden, plus the
    // overrides. Names are case-SENSITIVE here (unlike the Windows peer).
    std::vector<std::string> envStore;
    {
        std::vector<std::string_view> overridden;
        overridden.reserve(envCount);
        for (unsigned long i = 0; i < envCount; ++i) {
            std::string_view e(envEntries[i]);
            overridden.push_back(e.substr(0, e.find('=')));
        }
        for (char** p = current_environ(); p && *p; ++p) {
            std::string_view e(*p);
            auto name = e.substr(0, e.find('='));
            if (std::ranges::find(overridden, name) != overridden.end()) continue;
            envStore.emplace_back(e);
        }
        for (unsigned long i = 0; i < envCount; ++i)
            envStore.emplace_back(envEntries[i]);
    }
    std::vector<char*> envp;
    envp.reserve(envStore.size() + 1);
    for (auto& s : envStore) envp.push_back(s.data());
    envp.push_back(nullptr);

    std::vector<char*> cargv;
    cargv.reserve(argvCount + 1);
    for (unsigned long i = 0; i < argvCount; ++i)
        cargv.push_back(const_cast<char*>(argvEntries[i]));
    cargv.push_back(nullptr);

    const bool capture = (sink != nullptr);
    // From the pipe to the parent's close of its write end: see LaunchSection.
    LaunchSection launch;
    int fds[2] = {-1, -1};
    if (capture && make_pipe(fds) != 0) return out;

    posix_spawn_file_actions_t fa;
    ::posix_spawn_file_actions_init(&fa);
    // Same cwd contract as the untimed launcher: a bounded child must land in
    // the same directory an unbounded one would, or adding a timeout would
    // silently move where a build program's relative writes go.
    if (cwd && *cwd)
        ::posix_spawn_file_actions_addchdir_np(&fa, cwd);
    if (capture) {
        ::posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
        ::posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
        ::posix_spawn_file_actions_addclose(&fa, fds[0]);
        ::posix_spawn_file_actions_addclose(&fa, fds[1]);
    }
    // else: no file actions for stdio at all — the child inherits ours, which
    // keeps its output live AND keeps it a terminal.

    posix_spawnattr_t attr;
    ::posix_spawnattr_init(&attr);
    if (ownGroup) {
        ::posix_spawnattr_setpgroup(&attr, 0);   // 0: a new group, id == pid
        ::posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    }
    pid_t pid = 0;
    int sp = ::posix_spawnp(&pid, cargv[0], &fa, &attr, cargv.data(), envp.data());
    ::posix_spawnattr_destroy(&attr);
    ::posix_spawn_file_actions_destroy(&fa);
    if (capture) ::close(fds[1]);
    launch.release();
    if (sp != 0) { out.spawn_error = sp; if (capture) ::close(fds[0]); return out; }
    if (ownGroup) guard_group_on_signal(pid);

    // Non-blocking reads so the deadline is still checked while the child is
    // quiet. A blocking read on a silent, hung child is exactly the hang this
    // whole mechanism exists to stop.
    if (capture)
        ::fcntl(fds[0], F_SETFL, ::fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK);

    const auto started = std::chrono::steady_clock::now();
    const auto until = deadlineMs > 0
        ? started + std::chrono::milliseconds(deadlineMs)
        : std::chrono::steady_clock::time_point::max();
    auto lastOutput = started;
    std::array<char, 4096> buf{};
    bool  killed = false;
    int   status = 0;

    auto drain = [&]() -> bool {
        if (!capture) return false;
        ssize_t n;
        bool any = false;
        for (int chunks = 0; chunks < 16 && (n = ::read(fds[0], buf.data(), buf.size())) > 0; ++chunks) {
            sink(ctx, buf.data(), static_cast<unsigned long>(n));
            any = true;
        }
        if (any) lastOutput = std::chrono::steady_clock::now();
        return any;
    };

    for (;;) {
        // Output can be continuous. Check budgets before draining another batch.
        if (!killed && (std::chrono::steady_clock::now() >= until || (stop && stop(ctx)))) {
            if (ownGroup) ::killpg(pid, SIGKILL);
            else ::kill(pid, SIGKILL);
            killed = true;
        }
        if (drain()) continue;

        pid_t r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            while (drain()) { /* tail — the child is gone, so this ends */ }
            break;
        }
        if (r < 0 && errno != EINTR && errno != ECHILD) break;

        const auto now = std::chrono::steady_clock::now();
        const bool overTotal = now >= until;
        const bool overIdle  = idleBound
            && now - lastOutput >= std::chrono::milliseconds(idleMs);
        if (!killed && (overTotal || overIdle)) {
            // The group when the child owns one: a shell's grandchild (the
            // program the shell ran) would otherwise keep running, and keep
            // the pipe open so the drain above never saw end-of-file.
            if (ownGroup) ::killpg(pid, SIGKILL);
            else          ::kill(pid, SIGKILL);
            killed = true;
            continue;
        }
        struct timespec ts{0, 20'000'000};   // 20ms
        ::nanosleep(&ts, nullptr);
    }
    if (ownGroup) unguard_group(pid);
    if (capture) ::close(fds[0]);

    out.exit_code = normalize_status(status);
    out.timed_out = killed;
    out.supported = true;
    return out;
}

// ─── Background children ─────────────────────────────────────────────────

namespace {

// Read by a signal handler, so `volatile sig_atomic_t` and nothing else: the
// handler may run between any two instructions and may not lock, allocate, or
// call anything that is not async-signal-safe. 0 means "nothing to clean up".
constexpr int kMaxGuardedGroups = 256;
volatile sig_atomic_t g_guardedGroups[kMaxGuardedGroups] = {};
// Serializes the WRITERS of the registry and of the handlers it installs. The
// handler never takes it.
std::mutex g_guardMutex;

// The files a signal leaves. A set is never freed while it may be read: the
// handler can run on another thread between the read of the pointer and its
// writes, so every set lives until exit, and `g_interrupt` only moves.
struct InterruptFiles {
    std::vector<std::string> paths, temps, contents;
};
std::atomic<InterruptFiles*> g_interrupt{nullptr};
std::vector<std::unique_ptr<InterruptFiles>> g_interruptSets;

void write_interrupt_files() {
    auto* set = g_interrupt.load();
    if (!set) return;
    for (std::size_t i = 0; i < set->paths.size(); ++i) {
        const int fd = ::open(set->temps[i].c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0) continue;
        const char* p = set->contents[i].data();
        std::size_t left = set->contents[i].size();
        while (left > 0) {
            const auto n = ::write(fd, p, left);
            if (n <= 0) break;
            p += n;
            left -= static_cast<std::size_t>(n);
        }
        ::close(fd);
        if (left == 0) ::rename(set->temps[i].c_str(), set->paths[i].c_str());
        else ::unlink(set->temps[i].c_str());
    }
}

// Whether anything still needs the handler: a guarded group, the terminal's
// mode, or files to leave.
bool handler_needed();

// The platforms without `pipe2` (macOS) share one section across every child
// start.
std::mutex g_launchMutex;

// The terminal whose mode the handler restores (-1: none), and that mode.
volatile sig_atomic_t g_terminalFd = -1;
struct termios g_terminalMode{};

void install_handlers();
void uninstall_handlers();

extern "C" void background_signal_handler(int sig) {
    // killpg is async-signal-safe. SIGKILL rather than SIGTERM: this is the
    // path where mcpp is about to stop existing, and there is nobody left to
    // escalate if the group ignores the polite request.
    //
    // SIGKILL also settles a case SIGTERM does not. ninja records a signal in a
    // flag and acts on it only where it waits for a subprocess; a ninja with no
    // command running never reaches that check, so a polite signal is recorded
    // and never obeyed. Measured: an orphaned ninja spinning at 100% of a core
    // outlived the removal of the entire sandbox it belonged to.
    for (int i = 0; i < kMaxGuardedGroups; ++i) {
        const auto group = g_guardedGroups[i];
        if (group > 0) ::killpg(static_cast<pid_t>(group), SIGKILL);
    }
    write_interrupt_files();
    if (g_terminalFd >= 0) ::tcsetattr(g_terminalFd, TCSANOW, &g_terminalMode);
    // Die of the signal we were sent, so the exit status is the one the shell
    // and any outer script expect from a Ctrl-C.
    ::signal(sig, SIG_DFL);
    ::raise(sig);
}

} // namespace

void guard_broken_pipe() {
    ::signal(SIGPIPE, background_signal_handler);
}

// make_pipe: linux/bounded_process.cpp (pipe2) and macos/bounded_process.cpp.

LaunchSection::LaunchSection() {
    if constexpr (!mcpp::platform::is_linux) {
        g_launchMutex.lock();
        held_ = true;
    }
}

LaunchSection::~LaunchSection() { release(); }

void LaunchSection::release() {
    if (!held_) return;
    held_ = false;
    g_launchMutex.unlock();
}

BackgroundChild spawn_background(const char* const* argvEntries,
                                 unsigned long      argvCount,
                                 const char*        cwd,
                                 int                inheritStdio)
{
    BackgroundChild out;
    if (argvCount == 0 || !argvEntries) return out;

    std::vector<char*> cargv;
    cargv.reserve(argvCount + 1);
    for (unsigned long i = 0; i < argvCount; ++i)
        cargv.push_back(const_cast<char*>(argvEntries[i]));
    cargv.push_back(nullptr);

    posix_spawn_file_actions_t fa;
    ::posix_spawn_file_actions_init(&fa);
    if (cwd && *cwd)
        ::posix_spawn_file_actions_addchdir_np(&fa, cwd);
    if (!inheritStdio) {
        // /dev/null on all three: a spanning hook must not write into ninja's
        // output, and must not be able to block on a terminal read either.
        ::posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
        ::posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
        ::posix_spawn_file_actions_adddup2(&fa, 1, 2);
    }

    // POSIX_SPAWN_SETPGROUP with pgroup 0: the child becomes the leader of a
    // new group whose id is its pid. Standard POSIX, unlike SETSID.
    posix_spawnattr_t attr;
    ::posix_spawnattr_init(&attr);
    ::posix_spawnattr_setpgroup(&attr, 0);
    ::posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);

    pid_t pid = 0;
    int sp = 0;
    {
        // A child that holds no pipe of its own must still not be started in
        // the window another thread's pipe is open in (see LaunchSection).
        LaunchSection launch;
        sp = ::posix_spawnp(&pid, cargv[0], &fa, &attr,
                            cargv.data(), current_environ());
    }
    ::posix_spawn_file_actions_destroy(&fa);
    ::posix_spawnattr_destroy(&attr);
    if (sp != 0) return out;

    out.ok    = true;
    out.group = pid;
    return out;
}

// DOES NOT REAP, and that is the whole point.
//
// A zombie still holds its pid, so an unreaped leader is what keeps the GROUP
// id from being recycled — and `background_stop` signals that group. Reaping
// here would hand the id back to the kernel between the poll and the kill,
// which on a busy machine is how a stop lands on somebody else's process.
// waitid(WNOWAIT) answers "has it exited?" without giving the id back.
int background_running(long long group, int* exitCode) {
    if (group <= 0) return -1;
    siginfo_t info{};
    info.si_pid = 0;
    if (::waitid(P_PID, static_cast<id_t>(group), &info,
                 WEXITED | WNOHANG | WNOWAIT) != 0)
        return -1;
    if (info.si_pid == 0) return 1;
    if (exitCode)
        *exitCode = (info.si_code == CLD_EXITED)
                  ? info.si_status
                  : 128 + info.si_status;   // shell convention for a signal
    return 0;
}

void background_stop(long long group, long long graceMs) {
    if (group <= 0) return;
    const pid_t pgid = static_cast<pid_t>(group);

    // The group, not the pid. This is the line the design is about: `kill(pid)`
    // reaches only the shell mcpp started, and `sh -c 'player & wait'` makes
    // the player a grandchild — still holding the audio device afterwards.
    ::killpg(pgid, SIGTERM);

    const auto until = std::chrono::steady_clock::now()
                     + std::chrono::milliseconds(graceMs);
    while (background_running(group, nullptr) == 1
           && std::chrono::steady_clock::now() < until) {
        struct timespec ts{0, 10'000'000};   // 10ms
        ::nanosleep(&ts, nullptr);
    }

    // Unconditional, and BEFORE the reap: the leader may have gone politely
    // while something it forked has not, and the group is still addressable
    // only for as long as the unreaped leader holds the id.
    ::killpg(pgid, SIGKILL);
    int status = 0;
    ::waitpid(pgid, &status, 0);
}

void guard_group_on_signal(long long group) {
    if (group <= 0) return;
    {
        std::lock_guard lock(g_guardMutex);
        for (int i = 0; i < kMaxGuardedGroups; ++i) {
            if (g_guardedGroups[i] == 0) {
                g_guardedGroups[i] = static_cast<sig_atomic_t>(group);
                install_handlers();
                return;
            }
        }
    }
    // Out of slots. Say so rather than return silently: an unguarded group is
    // a process that outlives mcpp, and the whole point of this file is that
    // such a process is never acceptable.
    std::fputs("mcpp: internal: more than 256 concurrently guarded process "
               "groups; the newest is NOT guarded and may outlive mcpp\n",
               stderr);
}

// Removing the group this call owns, rather than clearing the array, is what
// makes concurrent owners safe: a nested or overlapping run must not disarm
// the guard an outer one still needs.
void unguard_group(long long group) {
    if (group <= 0) return;
    std::lock_guard lock(g_guardMutex);
    for (int i = 0; i < kMaxGuardedGroups; ++i)
        if (g_guardedGroups[i] == static_cast<sig_atomic_t>(group))
            g_guardedGroups[i] = 0;
    if (!handler_needed()) uninstall_handlers();
}

void clear_group_guard() {
    std::lock_guard lock(g_guardMutex);
    for (int i = 0; i < kMaxGuardedGroups; ++i) g_guardedGroups[i] = 0;
    if (!handler_needed()) uninstall_handlers();
}

bool group_guard_idle() {
    std::lock_guard lock(g_guardMutex);
    for (int i = 0; i < kMaxGuardedGroups; ++i)
        if (g_guardedGroups[i] != 0) return false;
    return true;
}

void guard_terminal_mode(int fd) {
    std::lock_guard lock(g_guardMutex);
    if (fd < 0 || ::tcgetattr(fd, &g_terminalMode) != 0) return;
    g_terminalFd = fd;
    install_handlers();
}

void unguard_terminal_mode() {
    std::lock_guard lock(g_guardMutex);
    if (g_terminalFd < 0) return;
    ::tcsetattr(g_terminalFd, TCSANOW, &g_terminalMode);
    g_terminalFd = -1;
    if (!handler_needed()) uninstall_handlers();
}

void set_interrupt_files(const std::vector<std::string>& paths,
                         const std::vector<std::string>& temps,
                         const std::vector<std::string>& contents) {
    if (paths.empty() || paths.size() > 4 || temps.size() != paths.size()
        || contents.size() != paths.size())
        return;
    std::lock_guard lock(g_guardMutex);
    g_interruptSets.push_back(std::make_unique<InterruptFiles>(InterruptFiles{paths, temps, contents}));
    g_interrupt.store(g_interruptSets.back().get());
    install_handlers();
}

void clear_interrupt_files() {
    std::lock_guard lock(g_guardMutex);
    g_interrupt.store(nullptr);
    if (!handler_needed()) uninstall_handlers();
}

namespace {
void install_handlers() {
    ::signal(SIGINT,  background_signal_handler);
    ::signal(SIGTERM, background_signal_handler);
    ::signal(SIGHUP,  background_signal_handler);
}
void uninstall_handlers() {
    ::signal(SIGINT,  SIG_DFL);
    ::signal(SIGTERM, SIG_DFL);
    ::signal(SIGHUP,  SIG_DFL);
}
bool handler_needed() {
    if (g_terminalFd >= 0 || g_interrupt.load()) return true;
    for (int i = 0; i < kMaxGuardedGroups; ++i)
        if (g_guardedGroups[i] != 0) return true;
    return false;
}
} // namespace

} // namespace mcpp::platform::unixproc
