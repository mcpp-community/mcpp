// mcpp.platform.unix.bounded_process — a child process with a deadline on
// POSIX. The peer of mcpp.platform.windows.bounded_process.
//
// WHY THE TWO SIDES ARE SEPARATE MODULES WITH ONE SHARED SIGNATURE
//
// They share no code — this is posix_spawn + a pipe + waitpid + SIGKILL, the
// other is CreateProcess + a Job object + WaitForSingleObject. What they share
// is a CONTRACT: "run this, capture its output, kill it at the deadline, and
// tell me whether you killed it". Putting that contract in one signature and
// each implementation behind its own module is what lets
// `mcpp.platform.process` dispatch once, with `if constexpr`, instead of
// carrying the platform question through twenty-five separate `#if` blocks.
//
// ⚠️ THE INTERFACE NAMES NO `std` TYPE — SAME HARD CONSTRAINT AS THE WINDOWS
// SIDE. See mcpp.platform.windows.bounded_process for the measurements: a new
// module imported by mcpp.platform.process whose EXPORTS mention std types
// corrupts every BMI downstream of it under GCC 16.1. `std` inside the module
// is fine; `std` in what it exports is not.
//
// The POSIX side does not strictly need the constraint (it is reached through
// the same dispatch, so it would be one more module in the same position — and
// the failure is silent enough that "probably fine" is not worth finding out).
// Keeping both sides identical also means the dispatcher marshals once, not
// twice.

module;

#if defined(__linux__) || defined(__APPLE__)
#ifndef _GNU_SOURCE
#define _GNU_SOURCE      // posix_spawn_file_actions_addchdir_np (glibc)
#endif
#include <unistd.h>      // pipe, close, read
#include <sys/wait.h>    // waitpid
#include <spawn.h>       // posix_spawnp, posix_spawn_file_actions_*
#include <signal.h>      // kill, SIGKILL
#include <cerrno>        // errno, EINTR
#include <fcntl.h>       // fcntl O_NONBLOCK
#include <time.h>        // nanosleep
#if defined(__APPLE__)
#include <crt_externs.h> // _NSGetEnviron
#endif
#endif

export module mcpp.platform.unix.bounded_process;

import std;

#if defined(__linux__)
// Declared in the module purview, not the global module fragment: an entity
// with C language linkage is never module-attached, so this names the same
// symbol the loader provides — and GCC rejects non-`#include` content in a
// global module fragment (-Wglobal-module).
extern "C" char **environ;
#endif

export namespace mcpp::platform::unixproc {

// Every member is a builtin type. Same reason as the Windows peer.
struct DeadlineRun {
    // False on every non-POSIX build, and on POSIX when the child could not be
    // spawned. "Could not spawn" and "ran and failed" must not share an exit
    // code, so the caller falls back rather than reporting a failure.
    bool supported = false;
    int  exit_code = 0;
    bool timed_out = false;
};

using OutputSink = void (*)(void* ctx, const char* data, unsigned long len);

// `argvEntries` is `argvCount` NUL-terminated strings; `envEntries` is
// `envCount` "KEY=VALUE" strings applied on top of the current environment.
// `cwd` may be null. A non-positive `deadlineMs` is rejected with
// supported=false: "no bound" belongs on the caller's untimed path.
//
// A NULL `sink` means "do not capture": the child INHERITS the caller's stdio
// and is still bounded. That is not an optimization — it is the
// `run_exec_deadline` contract. Routing an uncaptured run through a pipe would
// (a) delay every line until the child exits, which is the opposite of what a
// bounded `mcpp test` run is for, and (b) make the child's stdout a pipe
// rather than a terminal, so gtest and friends silently drop their colors.
DeadlineRun capture_with_deadline(const char* const* argvEntries,
                                  unsigned long      argvCount,
                                  const char* const* envEntries,
                                  unsigned long      envCount,
                                  const char*        cwd,
                                  long long          deadlineMs,
                                  OutputSink         sink,
                                  void*              ctx);

// ─── A child that outlives the call that started it (#496) ───────────────
//
// `capture_with_deadline` owns its child for the length of one call. A project
// `[hooks] during_build` command is owned for the length of the BUILD, so the
// caller needs a handle it can poll and stop later. Every member is a builtin,
// same constraint as DeadlineRun.
//
// ⚠️ `group`, NOT a pid. The child is placed in a process group of its own
// (posix_spawnattr_setpgroup) and stopped with killpg, because the thing being
// started is a user-authored SHELL command: `sh -c 'player & wait'` makes the
// writer a grandchild, and `kill(pid)` reaches only the shell. A background
// player that survives its build, from a process the user cannot name, is the
// worst outcome this API has — so the group is the unit throughout.
struct BackgroundChild {
    bool      ok    = false;
    long long group = 0;   // the child's process-group id (== its pid)
};

// `inheritStdio == 0` sends the child's output to /dev/null. A spanning hook
// writes CONCURRENTLY with ninja and would otherwise interleave into the middle
// of a compiler diagnostic.
BackgroundChild spawn_background(const char* const* argvEntries,
                                 unsigned long      argvCount,
                                 const char*        cwd,
                                 int                inheritStdio);

// 1 = still running, 0 = exited, -1 = unknown. When it returns 0, `exitCode`
// (if given) receives the shell convention: the status, or 128+signal.
//
// The code is part of the answer rather than a second call, because the only
// caller that needs it — the `loop` supervisor — has to distinguish "the
// player finished the track" from "the command does not exist". Restarting the
// first forever is the feature; restarting the second forever is a spin.
int background_running(long long group, int* exitCode);

// SIGTERM, `graceMs`, then SIGKILL — to the GROUP. Reaps the direct child.
void background_stop(long long group, long long graceMs);

// ─── Ctrl-C ──────────────────────────────────────────────────────────────
//
// Its own process group is what makes killpg possible AND what stops the
// terminal's SIGINT from reaching the child: Ctrl-C would kill mcpp and leave
// the player running. Both halves are required, so the group that must not
// outlive us is registered here for the duration.
//
// The handler does the minimum that is async-signal-safe: killpg (which is),
// then the default action.
void guard_group_on_signal(long long group);
void clear_group_guard();

} // namespace mcpp::platform::unixproc

namespace mcpp::platform::unixproc {

#if defined(__linux__) || defined(__APPLE__)

namespace {

char** current_environ() {
#if defined(__APPLE__)
    return *::_NSGetEnviron();
#else
    return environ;
#endif
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
                                  OutputSink         sink,
                                  void*              ctx)
{
    DeadlineRun out;
    if (deadlineMs <= 0 || argvCount == 0 || !argvEntries) return out;

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
    int fds[2] = {-1, -1};
    if (capture && ::pipe(fds) != 0) return out;

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

    pid_t pid = 0;
    int sp = ::posix_spawnp(&pid, cargv[0], &fa, nullptr, cargv.data(), envp.data());
    ::posix_spawn_file_actions_destroy(&fa);
    if (capture) ::close(fds[1]);
    if (sp != 0) { if (capture) ::close(fds[0]); return out; }

    // Non-blocking reads so the deadline is still checked while the child is
    // quiet. A blocking read on a silent, hung child is exactly the hang this
    // whole mechanism exists to stop.
    if (capture)
        ::fcntl(fds[0], F_SETFL, ::fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK);

    const auto until = std::chrono::steady_clock::now()
                     + std::chrono::milliseconds(deadlineMs);
    std::array<char, 4096> buf{};
    bool  killed = false;
    int   status = 0;

    auto drain = [&]() -> bool {
        if (!capture) return false;
        ssize_t n;
        bool any = false;
        while ((n = ::read(fds[0], buf.data(), buf.size())) > 0) {
            sink(ctx, buf.data(), static_cast<unsigned long>(n));
            any = true;
        }
        return any;
    };

    for (;;) {
        if (drain()) continue;

        pid_t r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            while (drain()) { /* tail — the child is gone, so this ends */ }
            break;
        }
        if (r < 0 && errno != EINTR && errno != ECHILD) break;

        if (!killed && std::chrono::steady_clock::now() >= until) {
            ::kill(pid, SIGKILL);
            killed = true;
            continue;
        }
        struct timespec ts{0, 20'000'000};   // 20ms
        ::nanosleep(&ts, nullptr);
    }
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
volatile sig_atomic_t g_guardedGroup = 0;

extern "C" void background_signal_handler(int sig) {
    const auto group = g_guardedGroup;
    // killpg is async-signal-safe. SIGKILL rather than SIGTERM: this is the
    // path where mcpp is about to stop existing, and there is nobody left to
    // escalate if the group ignores the polite request.
    if (group > 0) ::killpg(static_cast<pid_t>(group), SIGKILL);
    // Die of the signal we were sent, so the exit status is the one the shell
    // and any outer script expect from a Ctrl-C.
    ::signal(sig, SIG_DFL);
    ::raise(sig);
}

} // namespace

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
    const int sp = ::posix_spawnp(&pid, cargv[0], &fa, &attr,
                                  cargv.data(), current_environ());
    ::posix_spawn_file_actions_destroy(&fa);
    ::posix_spawnattr_destroy(&attr);
    if (sp != 0) return out;

    out.ok    = true;
    out.group = pid;
    return out;
}

// ⚠️ DOES NOT REAP, and that is the whole point.
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
    g_guardedGroup = static_cast<sig_atomic_t>(group);
    ::signal(SIGINT,  background_signal_handler);
    ::signal(SIGTERM, background_signal_handler);
    ::signal(SIGHUP,  background_signal_handler);
}

void clear_group_guard() {
    g_guardedGroup = 0;
    ::signal(SIGINT,  SIG_DFL);
    ::signal(SIGTERM, SIG_DFL);
    ::signal(SIGHUP,  SIG_DFL);
}

#else

DeadlineRun capture_with_deadline(const char* const*, unsigned long,
                                  const char* const*, unsigned long,
                                  const char*, long long, OutputSink, void*) {
    // Not POSIX: mcpp.platform.windows.bounded_process owns this.
    return {};
}

BackgroundChild spawn_background(const char* const*, unsigned long,
                                 const char*, int) {
    return {};
}
int  background_running(long long, int*) { return -1; }
void background_stop(long long, long long) {}
void guard_group_on_signal(long long) {}
void clear_group_guard() {}

#endif

} // namespace mcpp::platform::unixproc
