// mcpp.platform.process on Linux and macOS: the launchers that start a
// program directly with posix_spawn (#248), and the C library's pipe and
// wait-status primitives.
//
// The extra environment goes into the child's envp ONLY (merged_environ); it
// never enters mcpp's own environment nor a host /bin/sh. On Linux that is the
// exact fix for the newer-glibc `sh:` crash; on macOS the old shell path built
// `KEY='v' cd <cwd> && prog`, where POSIX binds the env assignments to `cd`
// alone — the real program (build.mcpp, the only env+cwd call site) received
// NO extra env and lost the whole MCPP_* contract. Direct exec also drops the
// shell quoting / signal / injection surface entirely. cwd is applied via
// posix_spawn_file_actions_addchdir_np, available on both glibc and macOS
// 10.15+ (mcpp's floor is macOS 14).
module;
#include <cerrno>      // errno after a refused execve (run_foreground)
#include <cstdio>
#include <fcntl.h>     // O_RDONLY, O_WRONLY for capture_stdout's /dev/null
#include <spawn.h>     // posix_spawnp, posix_spawn_file_actions_* (incl. addchdir_np)
#include <sys/stat.h>  // umask
#include <sys/wait.h>  // waitpid
#include <unistd.h>    // pipe, dup2, close, read

module mcpp.platform.process;

import std;
import mcpp.platform.common;
import mcpp.platform.env;
import mcpp.platform.unix.bounded_process;

namespace mcpp::platform::process {

void set_umask_022() {
    ::umask(022);
}

int normalize_exit_code(int rc) {
    if (WIFEXITED(rc))
        return WEXITSTATUS(rc);
    if (WIFSIGNALED(rc))
        return 128 + WTERMSIG(rc);
    return rc;
}

std::FILE* open_pipe(const std::string& command) { return ::popen(command.c_str(), "r"); }
int close_pipe(std::FILE* pipe) { return ::pclose(pipe); }

namespace {

char** host_environ() {
    return mcpp::platform::env::environment();
}

// An outer `mcpp run`/`mcpp test` points LD_LIBRARY_PATH at mcpp's private
// glibc payload so ITS child (a sandbox-linked user binary) can load. When
// that child spawns mcpp again (e.g. a course provider driving `mcpp test`),
// the same value would flow on into the inner mcpp's own children — and the
// sandbox ninja/gcc (host-glibc binaries) then resolve a MISMATCHED libc and
// segfault inside the dynamic linker before main (trace signature: a bare
// `__vdso_time` line). Strip exactly the private-glibc payload entries from
// inherited loader paths: user-supplied entries survive, and an `extra`
// override (the correct per-child value) always wins over the inherited var.
//
// The predicate itself lives in mcpp.platform.env, because the OTHER half of
// this guarantee is there: a composed override (dirs + inherited tail) arrives
// here as `extra` and therefore bypasses the sanitation below, so
// prepend_path_list has to sanitize the tail it carries.
using mcpp::platform::env::strip_private_glibc;

// Build a child environment block = the current environ with `extra` overrides
// applied. Returned vector owns the strings; the caller derives a NUL-terminated
// char* array from it. Built in the PARENT so the child env never requires a
// post-fork setenv and mcpp's own environment is never touched.
std::vector<std::string> merged_environ(
    const std::vector<std::pair<std::string, std::string>>& extra)
{
    std::vector<std::string> out;
    std::set<std::string> overridden;
    for (auto& [k, v] : extra) { out.push_back(k + "=" + v); overridden.insert(k); }
    for (char** e = host_environ(); e && *e; ++e) {
        std::string_view entry(*e);
        auto eq = entry.find('=');
        std::string key(eq == std::string_view::npos ? entry : entry.substr(0, eq));
        if (overridden.contains(key)) continue;
        if (eq != std::string_view::npos
            && (key == "LD_LIBRARY_PATH" || key == "DYLD_LIBRARY_PATH")) {
            auto cleaned = strip_private_glibc(entry.substr(eq + 1));
            if (!cleaned.empty()) out.push_back(key + "=" + cleaned);
            continue;   // nothing legitimate left → drop the var entirely
        }
        out.emplace_back(entry);
    }
    return out;
}

} // namespace

int run_exec(const std::vector<std::string>& argv,
             const std::vector<std::pair<std::string, std::string>>& extraEnv,
             int* spawn_error)
{
    if (spawn_error) *spawn_error = 0;
    if (argv.empty()) return 127;
    auto envStore = merged_environ(extraEnv);
    std::vector<char*> envp;
    for (auto& s : envStore) envp.push_back(s.data());
    envp.push_back(nullptr);
    std::vector<char*> cargv;
    for (auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);

    // THE CHILD GETS ITS OWN PROCESS GROUP, AND mcpp KILLS THAT GROUP IF IT IS
    // ITSELF KILLED.
    //
    // Without this, terminating mcpp leaves the child running. Measured: every
    // `timeout`-terminated `mcpp run` left an orphaned ninja spinning at 100%
    // of a core, and one of them outlived the removal of the entire sandbox it
    // belonged to — its working directory read `(deleted)` and it was still
    // burning a core half an hour later. Any CI that wraps mcpp in `timeout`
    // leaks a busy core per timeout.
    //
    // The group rather than the pid, because the child starts children of its
    // own: killing ninja alone would leave its compilers behind.
    //
    // A signal is not enough on its own, which is why the guard sends SIGKILL.
    // ninja records a signal in a flag and acts on it where it waits for a
    // subprocess; a ninja with no command running never reaches that check, so
    // a polite signal is recorded and never obeyed.
    posix_spawnattr_t attr;
    ::posix_spawnattr_init(&attr);
    ::posix_spawnattr_setpgroup(&attr, 0);       // 0 ⇒ new group, id == pid
    ::posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);

    pid_t pid = 0;
    int sp = 0;
    {
        // A child with no pipe of its own is still started outside the window
        // another thread's pipe is open in.
        LaunchSection launch;
        sp = ::posix_spawnp(&pid, cargv[0], nullptr, &attr, cargv.data(), envp.data());
    }
    ::posix_spawnattr_destroy(&attr);
    if (sp != 0) {
        // Reported once: by the caller when it asked for the errno, here
        // otherwise. Never dropped — the errno in hand at this line is the
        // whole difference between "Exec format error" and a blank line.
        if (spawn_error) *spawn_error = sp;
        else std::fputs(spawn_failure(argv.front(), sp).c_str(), stderr);
        return 127;
    }
    mcpp::platform::unixproc::guard_group_on_signal(pid);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) { /* EINTR retry */ }
    mcpp::platform::unixproc::unguard_group(pid);
    return normalize_exit_code(status);
}

int run_foreground(const std::vector<std::string>& argv,
                   const std::vector<std::pair<std::string, std::string>>& extraEnv,
                   int* spawn_error)
{
    if (spawn_error) *spawn_error = 0;
    if (argv.empty()) return 127;
    // A guarded group is a child mcpp still owns (ninja, a `during_build`
    // hook). Replacing mcpp would leave it without an owner, so that is an
    // internal error and not a silent orphan.
    if (!mcpp::platform::unixproc::group_guard_idle()) {
        std::fputs("mcpp: internal: a child of mcpp is still running; the program "
                   "is not started\n", stderr);
        return 125;
    }
    // The terminal's mode and the signal handlers mcpp installed are restored
    // here; execve then resets every handled signal to its default.
    mcpp::platform::unixproc::unguard_terminal_mode();
    mcpp::platform::unixproc::clear_group_guard();

    auto envStore = merged_environ(extraEnv);
    std::vector<char*> envp;
    for (auto& s : envStore) envp.push_back(s.data());
    envp.push_back(nullptr);
    std::vector<char*> cargv;
    for (auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);

    std::fflush(nullptr);
    // PATH is searched with mcpp's own PATH, as posix_spawnp searched it for
    // run_exec, and not with the program's environment.
    const std::string& file = argv.front();
    int err = ENOENT;
    if (file.find('/') != std::string::npos) {
        ::execve(file.c_str(), cargv.data(), envp.data());
        err = errno;
    } else {
        const char* path = std::getenv("PATH");
        std::string_view dirs = (path && *path) ? path : "/usr/bin:/bin";
        bool denied = false;
        for (auto dir : dirs | std::views::split(':')) {
            std::string candidate{std::string_view(dir)};
            if (candidate.empty()) candidate = ".";
            candidate += '/';
            candidate += file;
            ::execve(candidate.c_str(), cargv.data(), envp.data());
            // The search continues past a directory that cannot hold the
            // program; any other refusal names the program that was found.
            if (errno == EACCES) { denied = true; continue; }
            if (errno != ENOENT && errno != ENOTDIR) { err = errno; denied = false; break; }
        }
        if (denied && err == ENOENT) err = EACCES;
    }
    if (spawn_error) *spawn_error = err;
    else std::fputs(spawn_failure(file, err).c_str(), stderr);
    return 127;
}

RunResult capture_exec(
    const std::vector<std::string>& argv,
    const std::vector<std::pair<std::string, std::string>>& extraEnv,
    std::string_view cwd,
    int* spawn_error)
{
    RunResult result;
    if (spawn_error) *spawn_error = 0;
    if (argv.empty()) { result.exit_code = 127; return result; }
    // posix_spawn + a pipe; stdout and stderr both go to the pipe so the
    // captured text is combined (replaces the old `2>&1`).
    auto envStore = merged_environ(extraEnv);
    std::vector<char*> envp;
    for (auto& s : envStore) envp.push_back(s.data());
    envp.push_back(nullptr);
    std::vector<char*> cargv;
    for (auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);

    // Several threads call this at once (a workspace's build programs are
    // compiled together). The pipe is close-on-exec, so a child another thread
    // starts while this one is open does not keep it open (see
    // mcpp.platform.unix.bounded_process for the rule and for what a platform
    // without `pipe2` does in addition).
    LaunchSection launch;
    int fds[2];
    if (mcpp::platform::unixproc::make_pipe(fds) != 0) {
        result.exit_code = 127;
        return result;
    }

    posix_spawn_file_actions_t fa;
    ::posix_spawn_file_actions_init(&fa);
    // Run the child in `cwd` when requested (e.g. build.mcpp, whose relative
    // file writes must land in the project root regardless of mcpp's own cwd).
    std::string cwdStore(cwd);
    if (!cwdStore.empty())
        ::posix_spawn_file_actions_addchdir_np(&fa, cwdStore.c_str());
    ::posix_spawn_file_actions_adddup2(&fa, fds[1], 1);  // stdout → pipe
    ::posix_spawn_file_actions_adddup2(&fa, fds[1], 2);  // stderr → same pipe
    ::posix_spawn_file_actions_addclose(&fa, fds[0]);
    ::posix_spawn_file_actions_addclose(&fa, fds[1]);

    // Owned exactly as `run_exec`'s child is, and for the same reason: this is
    // the launcher a FULL build uses, so a `mcpp build` interrupted here is the
    // common case rather than the rare one. Fixing only `run_exec` left the
    // orphan in place — measured, with the two launchers giving opposite
    // answers to the same test.
    posix_spawnattr_t attr;
    ::posix_spawnattr_init(&attr);
    ::posix_spawnattr_setpgroup(&attr, 0);
    ::posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);

    pid_t pid = 0;
    int sp = ::posix_spawnp(&pid, cargv[0], &fa, &attr, cargv.data(), envp.data());
    ::posix_spawnattr_destroy(&attr);
    ::posix_spawn_file_actions_destroy(&fa);
    ::close(fds[1]);
    launch.release();
    if (sp == 0) mcpp::platform::unixproc::guard_group_on_signal(pid);
    if (sp != 0) {
        ::close(fds[0]);
        result.exit_code = 127;
        if (spawn_error) *spawn_error = sp;
        else result.output = spawn_failure(argv.front(), sp);
        return result;
    }

    std::array<char, 4096> buf{};
    ssize_t n;
    while ((n = ::read(fds[0], buf.data(), buf.size())) > 0)
        result.output.append(buf.data(), static_cast<size_t>(n));
    ::close(fds[0]);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) { /* EINTR retry */ }
    mcpp::platform::unixproc::unguard_group(pid);
    result.exit_code = normalize_exit_code(status);
    return result;
}

RunResult capture_stdout(
    const std::vector<std::string>& argv,
    const std::vector<std::pair<std::string, std::string>>& extraEnv,
    int* spawn_error)
{
    RunResult result;
    if (spawn_error) *spawn_error = 0;
    if (argv.empty()) { result.exit_code = 127; return result; }
    auto envStore = merged_environ(extraEnv);
    std::vector<char*> envp;
    for (auto& s : envStore) envp.push_back(s.data());
    envp.push_back(nullptr);
    std::vector<char*> cargv;
    for (auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);

    // The same rule as capture_exec's.
    LaunchSection launch;
    int fds[2];
    if (mcpp::platform::unixproc::make_pipe(fds) != 0) {
        result.exit_code = 127;
        return result;
    }

    posix_spawn_file_actions_t fa;
    ::posix_spawn_file_actions_init(&fa);
    ::posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    ::posix_spawn_file_actions_adddup2(&fa, fds[1], 1);   // stdout -> pipe
    ::posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    ::posix_spawn_file_actions_addclose(&fa, fds[0]);
    ::posix_spawn_file_actions_addclose(&fa, fds[1]);

    // Owned as capture_exec's child is: a probe that outlives an interrupted
    // mcpp is the same orphan, only smaller.
    posix_spawnattr_t attr;
    ::posix_spawnattr_init(&attr);
    ::posix_spawnattr_setpgroup(&attr, 0);
    ::posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);

    pid_t pid = 0;
    int sp = ::posix_spawnp(&pid, cargv[0], &fa, &attr, cargv.data(), envp.data());
    ::posix_spawnattr_destroy(&attr);
    ::posix_spawn_file_actions_destroy(&fa);
    ::close(fds[1]);
    launch.release();
    if (sp != 0) {
        ::close(fds[0]);
        result.exit_code = 127;
        if (spawn_error) *spawn_error = sp;
        return result;
    }
    mcpp::platform::unixproc::guard_group_on_signal(pid);

    std::array<char, 4096> buf{};
    ssize_t n;
    while ((n = ::read(fds[0], buf.data(), buf.size())) > 0)
        result.output.append(buf.data(), static_cast<size_t>(n));
    ::close(fds[0]);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) { /* EINTR retry */ }
    mcpp::platform::unixproc::unguard_group(pid);
    result.exit_code = normalize_exit_code(status);
    return result;
}

} // namespace mcpp::platform::process
