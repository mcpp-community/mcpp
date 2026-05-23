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
#include <cstdio>
#include <cstdlib>
#if defined(_WIN32)
#include <stdlib.h>    // _putenv_s
#define popen  _popen
#define pclose _pclose
#endif

export module mcpp.platform.process;

import std;

export namespace mcpp::platform::process {

struct RunResult {
    int         exit_code = 0;
    std::string output;
};

// Run `command` via the platform shell, capture stdout.
// On POSIX, stdin is automatically redirected from /dev/null.
RunResult capture(std::string_view command);

// Run `command` with extra environment variables (additive).
// Windows: _putenv_s (mutates calling process env).
// POSIX: prefixes command with VAR=val tokens (no mutation).
RunResult capture_with_env(
    std::string_view command,
    const std::vector<std::pair<std::string, std::string>>& env);

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

} // namespace mcpp::platform::process

// ─── Implementation ──────────────────────────────────────────────────────

namespace mcpp::platform::process {

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

int normalize_exit_code(int rc) {
#if defined(_WIN32)
    return rc;
#else
    if (WIFEXITED(rc))
        return WEXITSTATUS(rc);
    return rc;
#endif
}

} // namespace

int extract_exit_code(int raw_status) {
    return normalize_exit_code(raw_status);
}

RunResult capture(std::string_view command) {
    auto cmd = seal_stdin(command);
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
    auto cmd = seal_stdin(command);
    return normalize_exit_code(std::system(cmd.c_str()));
}

int run_streaming(std::string_view command,
                  std::function<void(std::string_view line)> on_line)
{
    auto cmd = seal_stdin(command);
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
    auto cmd = seal_stdin(command);
    std::FILE* fp = ::popen(cmd.c_str(), "r");
    if (!fp) return -1;

    std::array<char, 8192> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), fp) != nullptr) {
        if (output) *output += buf.data();
        std::fputs(buf.data(), stdout);
    }
    return normalize_exit_code(::pclose(fp));
}

} // namespace mcpp::platform::process
