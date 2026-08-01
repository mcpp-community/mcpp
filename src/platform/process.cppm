// mcpp.platform.process — platform-aware process runner.
//
// Centralises all popen/system usage so callers do not scatter #if _WIN32
// guards or duplicate the popen-read loop.  All functions automatically
// seal stdin (redirect from /dev/null on POSIX, from NUL on Windows) to
// prevent interactive prompts from child processes:
//   - POSIX: fixes macOS first-run hangs where xcrun / xcode-select would
//     block waiting for user input.
//   - Windows: fixes first-run hangs where xlings / xim / curl / git child
//     processes would block on terminal stdin, forcing the user to press
//     Enter repeatedly to advance bootstrap / toolchain install.
//
// Entry points:
//   capture        — run a command, capture stdout
//   run_silent     — run a command, discard output
//   run_streaming  — run a command, stream stdout line by line
//
// NOTE: These functions run commands through the platform shell (sh/cmd.exe).
// Callers are responsible for shell-quoting arguments (see platform.shell).

module;
#ifndef _GNU_SOURCE
#define _GNU_SOURCE        // for posix_spawn_file_actions_addchdir_np (glibc)
#endif
#include <cstdio>
#include <cstdlib>
#if defined(_WIN32)
#include <stdlib.h>    // _putenv_s
#define popen  _popen
#define pclose _pclose
#elif defined(__linux__) || defined(__APPLE__)
// Linux and macOS launchers do a direct exec (see run_exec / capture_exec
// below); only Windows keeps the std::system shell path (#248).
#include <unistd.h>    // pipe, dup2, close, read
#include <sys/wait.h>  // waitpid
#include <spawn.h>     // posix_spawnp, posix_spawn_file_actions_* (incl. addchdir_np)
#include <signal.h>    // kill, SIGKILL (deadline runners)
#include <cerrno>      // errno, EINTR (deadline wait loop)
#include <poll.h>      // poll (deadline capture)
#include <fcntl.h>     // fcntl O_NONBLOCK (deadline capture)
#include <time.h>      // nanosleep (deadline wait loop)
#if defined(__APPLE__)
#include <crt_externs.h>  // _NSGetEnviron — direct `environ` is only linkable
                          // from executables on Apple, not from dylibs
#else
extern "C" char **environ;
#endif
#endif

export module mcpp.platform.process;

import std;
import mcpp.platform.env;
import mcpp.platform.shell;

export namespace mcpp::platform::process {

struct RunResult {
    int         exit_code = 0;
    std::string output;
};

// Run `command` via the platform shell, capture stdout.
// On POSIX, stdin is automatically redirected from /dev/null.
RunResult capture(std::string_view command);

// Run a host tool while clearing target runtime library search variables.
// This prevents target/program LD_LIBRARY_PATH from poisoning system tools
// such as sha256sum, compiler probes, env, or the shell itself.
RunResult capture_host_tool(std::string_view command);

// Run `command` with extra environment variables (additive).
// Windows: _putenv_s (mutates calling process env).
// POSIX: prefixes command with VAR=val tokens (no mutation).
RunResult capture_with_env(
    std::string_view command,
    const std::vector<std::pair<std::string, std::string>>& env);

// Launch a program DIRECTLY (no shell), inheriting stdio. argv[0] is the
// program (PATH-searched). `extraEnv` is applied to the CHILD ONLY — the
// calling process environment is never mutated, so a target's loader vars
// (LD_LIBRARY_PATH) cannot poison mcpp itself or any sibling host process.
// Returns a platform-normalized exit code, or 127 if exec fails.
int run_exec(const std::vector<std::string>& argv,
             const std::vector<std::pair<std::string, std::string>>& extraEnv = {});

// Same as run_exec but captures stdout AND stderr combined (replaces the old
// `… 2>&1` redirect) into RunResult::output. Required because the only consumer
// (ninja fast-path) parses error text — which ninja writes to stderr — via
// is_stale_ninja_failure / filter_ninja_output. No shell → no quoting/injection.
RunResult capture_exec(
    const std::vector<std::string>& argv,
    const std::vector<std::pair<std::string, std::string>>& extraEnv = {},
    std::string_view cwd = {});

// Deadline variants (POSIX): kill the child with SIGKILL once `deadline`
// elapses and set *timed_out. A zero deadline means no limit. On Windows the
// deadline is currently ignored (no supported kill-by-handle path in the
// residual shell launcher) — callers must treat the timeout as best-effort.
int run_exec_deadline(const std::vector<std::string>& argv,
                      const std::vector<std::pair<std::string, std::string>>& extraEnv,
                      std::chrono::milliseconds deadline,
                      bool* timed_out);

RunResult capture_exec_deadline(
    const std::vector<std::string>& argv,
    const std::vector<std::pair<std::string, std::string>>& extraEnv,
    std::chrono::milliseconds deadline,
    bool* timed_out);

// Run `command` silently (discard stdout/stderr).
// On POSIX, stdin is automatically redirected from /dev/null.
int run_silent(std::string_view command);

// Run `command`, stream stdout line-by-line via callback.
// On POSIX, stdin is automatically redirected from /dev/null.
int run_streaming(std::string_view command,
                  std::function<void(std::string_view line)> on_line);

// Run `command`, passing stdout/stderr through to the terminal.
// Optionally captures stdout into `output` if non-null.
// Returns a platform-normalized exit code (WEXITSTATUS on POSIX).
int run_passthrough(std::string_view command,
                    std::string* output = nullptr);

// Extract a platform-normalized exit code from a raw system()/pclose()
// return value.  Windows returns the exit code directly; POSIX returns
// a wait-status word requiring WIFEXITED/WEXITSTATUS unwrapping.
int extract_exit_code(int raw_status);

// ─── Windows command-line shaping (host-independent, for testing) ─────────
//
// `cmd.exe /c <string>` applies a quote rule that silently mangles most
// command lines (`cmd /?`, /C section): unless the whole string is exactly
// one quoted executable name, cmd removes the FIRST character and the LAST
// quote character, then runs the remainder. A correctly quoted line like
//
//     "C:\Program Files\gcc\g++.exe" -c "main.cpp"
//
// therefore arrives as
//
//     C:\Program Files\gcc\g++.exe" -c "main.cpp
//
// The fix is to hand cmd an outer pair to consume. These two functions build
// exactly that shape and are compiled on every platform so the rule can be
// unit-tested from Linux/macOS — the Windows branch below is otherwise
// unreachable in every environment mcpp is normally developed on, which is
// how the unquoted-argv[0] bug survived.
std::string windows_command_from_argv(const std::vector<std::string>& argv);
std::string windows_wrap_for_cmd_c(std::string_view cmd);

} // namespace mcpp::platform::process

// ─── Implementation ──────────────────────────────────────────────────────

namespace mcpp::platform::process {

// Host-independent (see the declarations): always the Windows shape.
std::string windows_command_from_argv(const std::vector<std::string>& argv) {
    if (argv.empty()) return "";
    std::string cmd = mcpp::platform::shell::quote_windows(argv[0]);
    for (std::size_t i = 1; i < argv.size(); ++i) {
        cmd += ' ';
        cmd += mcpp::platform::shell::quote_windows(argv[i]);
    }
    return cmd;
}

std::string windows_wrap_for_cmd_c(std::string_view cmd) {
    return "\"" + std::string(cmd) + "\"";
}

namespace {

// Append a non-interactive stdin redirect to prevent child processes from
// blocking on terminal input.
//   - POSIX:  "< /dev/null"  — fixes macOS xcrun / xcode-select hangs.
//   - Windows: "<NUL"        — fixes xlings / xim / curl / git hangs on
//                              first-run toolchain install (user otherwise
//                              had to press Enter repeatedly to advance).
// `cmd.exe` accepts `<NUL` as a redirect for an immediately-EOF stdin.
std::string seal_stdin(std::string_view cmd) {
#if defined(_WIN32)
    return std::string(cmd) + " <NUL";
#else
    return std::string(cmd) + " </dev/null";
#endif
}

// Everything that reaches _popen / std::system on Windows is run by
// `cmd.exe /c <string>`, and cmd applies a quote rule that mangles any
// command line carrying more than one pair of quotes (`cmd /?`, the /C
// section): unless the whole string is exactly one quoted executable name,
// cmd strips the FIRST character and the LAST quote character and runs what
// is left. So
//
//     "C:\Program Files\gcc\g++.exe" -c "main.cpp"
//
// becomes
//
//     C:\Program Files\gcc\g++.exe" -c "main.cpp
//
// which is why command_from_argv used to leave argv[0] unquoted — the
// program path then survived, at the cost of breaking as soon as it
// contained a space, which every default install path does
// (`C:\Program Files\...`, or any user whose account name has a space).
//
// The documented fix is to give cmd an outer pair to eat, so the inner
// quoting arrives intact. Applied at the single point where a command
// string becomes a child process, so no caller has to remember it, and the
// redirects appended by seal_stdin / silent_redirect stay inside the wrap
// where cmd still parses them after stripping.
std::string wrap_for_cmd_c(std::string_view cmd) {
#if defined(_WIN32)
    return windows_wrap_for_cmd_c(cmd);
#else
    return std::string(cmd);
#endif
}

// Seal stdin AND wrap. Kept separate from wrap_for_cmd_c because run_exec
// deliberately inherits stdio — `mcpp run` hands the terminal to the program
// being run, and sealing its stdin would break every interactive one.
std::string finalize_shell_command(std::string_view cmd) {
    return wrap_for_cmd_c(seal_stdin(cmd));
}

int normalize_exit_code(int rc) {
#if defined(_WIN32)
    return rc;
#else
    if (WIFEXITED(rc))
        return WEXITSTATUS(rc);
    // Shell convention for signaled children: 128 + signal number. The raw
    // wait-status word only *happens* to look right when the core-dump bit
    // is set (SIGSEGV+core → 0x8B = 139); without it a SIGTERM death would
    // surface as "exit 15" and be indistinguishable from a normal exit code.
    if (WIFSIGNALED(rc))
        return 128 + WTERMSIG(rc);
    return rc;
#endif
}

#if defined(__linux__) || defined(__APPLE__)
// Portable accessor for the host environment block. On Apple, `environ` is
// only linkable from executables (not dylibs), so _NSGetEnviron() is the
// sanctioned spelling; Linux keeps the plain `environ` symbol.
char** host_environ() {
#if defined(__APPLE__)
    return *::_NSGetEnviron();
#else
    return environ;
#endif
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

std::string spawn_failure(std::string_view program, int error) {
    return std::format("posix_spawnp('{}') failed (error {}): {}\n",
                       program, error, std::generic_category().message(error));
}
#else
// Build a shell command line from an argv vector (Windows + residual non-POSIX
// fallback only; Linux/macOS exec directly, #248). EVERY token is shell-quoted,
// including the program — a payload under `C:\Program Files\...` or a home
// directory with a space in the user name is otherwise cut at the first space
// and reported as `'C:\Program' is not recognized`.
//
// argv[0] used to be left raw here to survive cmd.exe's /c quote stripping.
// That traded one bug for another; finalize_shell_command now feeds cmd the
// outer quote pair it insists on eating, so the quoting below arrives intact.
std::string command_from_argv(const std::vector<std::string>& argv) {
#if defined(_WIN32)
    // One derivation: the tested, host-independent shaper above.
    return windows_command_from_argv(argv);
#else
    if (argv.empty()) return "";
    std::string cmd = mcpp::platform::shell::quote(argv[0]);
    for (std::size_t i = 1; i < argv.size(); ++i) {
        cmd += ' ';
        cmd += mcpp::platform::shell::quote(argv[i]);
    }
    return cmd;
#endif
}
#endif

} // namespace

int extract_exit_code(int raw_status) {
    return normalize_exit_code(raw_status);
}

RunResult capture(std::string_view command) {
    auto cmd = finalize_shell_command(command);
    RunResult result;

    std::FILE* fp = ::popen(cmd.c_str(), "r");
    if (!fp) {
        result.exit_code = -1;
        return result;
    }

    std::array<char, 4096> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), fp) != nullptr)
        result.output += buf.data();

    result.exit_code = normalize_exit_code(::pclose(fp));
    return result;
}

RunResult capture_host_tool(std::string_view command) {
    auto key = mcpp::platform::env::host_tool_runtime_library_path_key();
    std::optional<mcpp::platform::env::ScopedEnv> runtime_env;
    if (!key.empty())
        runtime_env.emplace(key, std::nullopt);
    return capture(command);
}

RunResult capture_with_env(
    std::string_view command,
    const std::vector<std::pair<std::string, std::string>>& env)
{
#if defined(_WIN32)
    for (auto& [k, v] : env)
        _putenv_s(k.c_str(), v.c_str());
    return capture(command);
#else
    std::string prefixed;
    for (auto& [k, v] : env) {
        prefixed += k;
        prefixed += '=';
        // Simple quoting for env values
        prefixed += '\'';
        for (char c : v) {
            if (c == '\'') prefixed += "'\\''";
            else prefixed += c;
        }
        prefixed += '\'';
        prefixed += ' ';
    }
    prefixed += command;
    return capture(prefixed);
#endif
}

int run_silent(std::string_view command) {
    auto cmd = finalize_shell_command(command);
    return normalize_exit_code(std::system(cmd.c_str()));
}

int run_streaming(std::string_view command,
                  std::function<void(std::string_view line)> on_line)
{
    auto cmd = finalize_shell_command(command);
    std::FILE* fp = ::popen(cmd.c_str(), "r");
    if (!fp) return -1;

    std::array<char, 16384> buf{};
    std::string acc;
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), fp) != nullptr) {
        acc += buf.data();
        std::size_t pos;
        while ((pos = acc.find('\n')) != std::string::npos) {
            if (on_line) {
                auto line = std::string_view{acc}.substr(0, pos);
                while (!line.empty() && line.back() == '\r')
                    line.remove_suffix(1);
                on_line(line);
            }
            acc.erase(0, pos + 1);
        }
    }
    if (!acc.empty() && on_line) {
        std::string_view line{acc};
        while (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        if (!line.empty()) on_line(line);
    }
    return normalize_exit_code(::pclose(fp));
}

int run_passthrough(std::string_view command, std::string* output) {
    auto cmd = finalize_shell_command(command);
    std::FILE* fp = ::popen(cmd.c_str(), "r");
    if (!fp) return -1;

    std::array<char, 8192> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), fp) != nullptr) {
        if (output) *output += buf.data();
        std::fputs(buf.data(), stdout);
    }
    return normalize_exit_code(::pclose(fp));
}

// run_exec / capture_exec are split by platform on purpose:
//
//   Linux /
//   macOS   — DIRECT exec via posix_spawn (unified in #248). The extra env goes
//             into the child's envp ONLY (merged_environ); it never enters
//             mcpp's own environment nor a host /bin/sh. On Linux that is the
//             exact fix for the newer-glibc `sh:` crash; on macOS the old shell
//             path built `KEY='v' cd <cwd> && prog`, where POSIX binds the env
//             assignments to `cd` alone — the real program (build.mcpp, the
//             only env+cwd call site) received NO extra env and lost the whole
//             MCPP_* contract. Direct exec also drops the shell quoting /
//             signal / injection surface entirely. cwd is applied via
//             posix_spawn_file_actions_addchdir_np, available on both glibc
//             and macOS 10.15+ (mcpp's floor is macOS 14).
//   Windows — KEEP the proven std::system shell path. The env-binding hazard
//             does not exist here (env goes through _putenv_s, not a prefix),
//             so we deliberately do not swap the launch primitive on a platform
//             we cannot iterate on locally.
//
// TODO(launcher-unify): Windows is the remaining exception; if it ever needs
// child-only env isolation, move it onto a CreateProcess/_spawn equivalent and
// delete the residual shell branch below.
int run_exec(const std::vector<std::string>& argv,
             const std::vector<std::pair<std::string, std::string>>& extraEnv)
{
    if (argv.empty()) return 127;
#if defined(__linux__) || defined(__APPLE__)
    auto envStore = merged_environ(extraEnv);
    std::vector<char*> envp;
    for (auto& s : envStore) envp.push_back(s.data());
    envp.push_back(nullptr);
    std::vector<char*> cargv;
    for (auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);

    pid_t pid = 0;
    if (::posix_spawnp(&pid, cargv[0], nullptr, nullptr, cargv.data(), envp.data()) != 0)
        return 127;  // spawn failed (e.g. program not found)
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) { /* EINTR retry */ }
    return normalize_exit_code(status);
#else
    std::string prefix = mcpp::platform::env::build_env_prefix(extraEnv);
    // wrap only — run_exec inherits stdio on purpose (see finalize_shell_command).
    std::string cmd = wrap_for_cmd_c(prefix + command_from_argv(argv));
    return normalize_exit_code(std::system(cmd.c_str()));
#endif
}

RunResult capture_exec(
    const std::vector<std::string>& argv,
    const std::vector<std::pair<std::string, std::string>>& extraEnv,
    std::string_view cwd)
{
    RunResult result;
    if (argv.empty()) { result.exit_code = 127; return result; }
#if defined(__linux__) || defined(__APPLE__)
    // posix_spawn + a pipe; stdout and stderr both go to the pipe so the
    // captured text is combined (replaces the old `2>&1`).
    int fds[2];
    if (::pipe(fds) != 0) { result.exit_code = 127; return result; }

    auto envStore = merged_environ(extraEnv);
    std::vector<char*> envp;
    for (auto& s : envStore) envp.push_back(s.data());
    envp.push_back(nullptr);
    std::vector<char*> cargv;
    for (auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);

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

    pid_t pid = 0;
    int sp = ::posix_spawnp(&pid, cargv[0], &fa, nullptr, cargv.data(), envp.data());
    ::posix_spawn_file_actions_destroy(&fa);
    ::close(fds[1]);
    if (sp != 0) {
        ::close(fds[0]);
        result.exit_code = 127;
        result.output = spawn_failure(argv.front(), sp);
        return result;
    }

    std::array<char, 4096> buf{};
    ssize_t n;
    while ((n = ::read(fds[0], buf.data(), buf.size())) > 0)
        result.output.append(buf.data(), static_cast<size_t>(n));
    ::close(fds[0]);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) { /* EINTR retry */ }
    result.exit_code = normalize_exit_code(status);
    return result;
#else
    std::string cmd = command_from_argv(argv) + " 2>&1";
    if (!cwd.empty()) {
#if defined(_WIN32)
        // cmd.exe `cd` without /d does not switch drives — a project on a
        // different drive than mcpp's own cwd would run the child (the
        // build.mcpp contract's only cwd consumer) in the wrong directory.
        cmd = "cd /d " + mcpp::platform::shell::quote(cwd) + " && " + cmd;
#else
        cmd = "cd " + mcpp::platform::shell::quote(cwd) + " && " + cmd;
#endif
    }
    return capture_with_env(cmd, extraEnv);
#endif
}

int run_exec_deadline(const std::vector<std::string>& argv,
                      const std::vector<std::pair<std::string, std::string>>& extraEnv,
                      std::chrono::milliseconds deadline,
                      bool* timed_out)
{
    if (timed_out) *timed_out = false;
    if (deadline.count() <= 0) return run_exec(argv, extraEnv);
    if (argv.empty()) return 127;
#if defined(__linux__) || defined(__APPLE__)
    auto envStore = merged_environ(extraEnv);
    std::vector<char*> envp;
    for (auto& s : envStore) envp.push_back(s.data());
    envp.push_back(nullptr);
    std::vector<char*> cargv;
    for (auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);

    pid_t pid = 0;
    if (::posix_spawnp(&pid, cargv[0], nullptr, nullptr, cargv.data(), envp.data()) != 0)
        return 127;

    auto until = std::chrono::steady_clock::now() + deadline;
    int status = 0;
    for (;;) {
        pid_t r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid) return normalize_exit_code(status);
        if (r < 0 && errno != EINTR) return 127;
        if (std::chrono::steady_clock::now() >= until) {
            ::kill(pid, SIGKILL);
            while (::waitpid(pid, &status, 0) < 0) { /* EINTR retry */ }
            if (timed_out) *timed_out = true;
            return normalize_exit_code(status);
        }
        struct timespec ts{0, 20'000'000};   // 20ms
        ::nanosleep(&ts, nullptr);
    }
#else
    // Windows: the residual shell launcher has no kill-by-handle path yet —
    // run untimed (documented best-effort semantics).
    return run_exec(argv, extraEnv);
#endif
}

RunResult capture_exec_deadline(
    const std::vector<std::string>& argv,
    const std::vector<std::pair<std::string, std::string>>& extraEnv,
    std::chrono::milliseconds deadline,
    bool* timed_out)
{
    if (timed_out) *timed_out = false;
    if (deadline.count() <= 0) return capture_exec(argv, extraEnv);
    RunResult result;
    if (argv.empty()) { result.exit_code = 127; return result; }
#if defined(__linux__) || defined(__APPLE__)
    int fds[2];
    if (::pipe(fds) != 0) { result.exit_code = 127; return result; }

    auto envStore = merged_environ(extraEnv);
    std::vector<char*> envp;
    for (auto& s : envStore) envp.push_back(s.data());
    envp.push_back(nullptr);
    std::vector<char*> cargv;
    for (auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
    cargv.push_back(nullptr);

    posix_spawn_file_actions_t fa;
    ::posix_spawn_file_actions_init(&fa);
    ::posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    ::posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
    ::posix_spawn_file_actions_addclose(&fa, fds[0]);
    ::posix_spawn_file_actions_addclose(&fa, fds[1]);

    pid_t pid = 0;
    int sp = ::posix_spawnp(&pid, cargv[0], &fa, nullptr, cargv.data(), envp.data());
    ::posix_spawn_file_actions_destroy(&fa);
    ::close(fds[1]);
    if (sp != 0) {
        ::close(fds[0]);
        result.exit_code = 127;
        result.output = spawn_failure(argv.front(), sp);
        return result;
    }

    ::fcntl(fds[0], F_SETFL, ::fcntl(fds[0], F_GETFL) | O_NONBLOCK);

    auto until = std::chrono::steady_clock::now() + deadline;
    bool killed = false;
    std::array<char, 4096> buf{};
    for (;;) {
        struct pollfd pfd{fds[0], POLLIN, 0};
        ::poll(&pfd, 1, 50);
        for (;;) {
            ssize_t n = ::read(fds[0], buf.data(), buf.size());
            if (n > 0) { result.output.append(buf.data(), static_cast<size_t>(n)); continue; }
            break;
        }
        int status = 0;
        pid_t r = ::waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            // Drain whatever is left in the pipe after exit.
            ssize_t n;
            while ((n = ::read(fds[0], buf.data(), buf.size())) > 0)
                result.output.append(buf.data(), static_cast<size_t>(n));
            ::close(fds[0]);
            result.exit_code = normalize_exit_code(status);
            if (timed_out) *timed_out = killed;
            return result;
        }
        if (!killed && std::chrono::steady_clock::now() >= until) {
            ::kill(pid, SIGKILL);
            killed = true;
        }
    }
#else
    return capture_exec(argv, extraEnv);
#endif
}

} // namespace mcpp::platform::process
