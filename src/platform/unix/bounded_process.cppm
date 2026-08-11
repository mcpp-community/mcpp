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
// When `sink` is null the output is discarded but the child is still bounded —
// that is the `run_exec_deadline` shape.
DeadlineRun capture_with_deadline(const char* const* argvEntries,
                                  unsigned long      argvCount,
                                  const char* const* envEntries,
                                  unsigned long      envCount,
                                  const char*        cwd,
                                  long long          deadlineMs,
                                  OutputSink         sink,
                                  void*              ctx);

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

    int fds[2];
    if (::pipe(fds) != 0) return out;

    posix_spawn_file_actions_t fa;
    ::posix_spawn_file_actions_init(&fa);
    // Same cwd contract as the untimed launcher: a bounded child must land in
    // the same directory an unbounded one would, or adding a timeout would
    // silently move where a build program's relative writes go.
    if (cwd && *cwd)
        ::posix_spawn_file_actions_addchdir_np(&fa, cwd);
    ::posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    ::posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
    ::posix_spawn_file_actions_addclose(&fa, fds[0]);
    ::posix_spawn_file_actions_addclose(&fa, fds[1]);

    pid_t pid = 0;
    int sp = ::posix_spawnp(&pid, cargv[0], &fa, nullptr, cargv.data(), envp.data());
    ::posix_spawn_file_actions_destroy(&fa);
    ::close(fds[1]);
    if (sp != 0) { ::close(fds[0]); return out; }

    // Non-blocking reads so the deadline is still checked while the child is
    // quiet. A blocking read on a silent, hung child is exactly the hang this
    // whole mechanism exists to stop.
    ::fcntl(fds[0], F_SETFL, ::fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK);

    const auto until = std::chrono::steady_clock::now()
                     + std::chrono::milliseconds(deadlineMs);
    std::array<char, 4096> buf{};
    bool  killed = false;
    int   status = 0;

    for (;;) {
        ssize_t n;
        bool drained = false;
        while ((n = ::read(fds[0], buf.data(), buf.size())) > 0) {
            if (sink) sink(ctx, buf.data(),
                           static_cast<unsigned long>(n));
            drained = true;
        }
        if (drained) continue;

        pid_t r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            // Drain the tail: the child is gone, so this terminates.
            while ((n = ::read(fds[0], buf.data(), buf.size())) > 0)
                if (sink) sink(ctx, buf.data(),
                               static_cast<unsigned long>(n));
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
    ::close(fds[0]);

    out.exit_code = normalize_status(status);
    out.timed_out = killed;
    out.supported = true;
    return out;
}

#else

DeadlineRun capture_with_deadline(const char* const*, unsigned long,
                                  const char* const*, unsigned long,
                                  const char*, long long, OutputSink, void*) {
    // Not POSIX: mcpp.platform.windows.bounded_process owns this.
    return {};
}

#endif

} // namespace mcpp::platform::unixproc
