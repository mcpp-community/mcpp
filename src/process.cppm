// mcpp.process — platform-aware process runner.
//
// Centralises all popen/system usage into a single module so callers do
// not need to scatter #if _WIN32 guards or duplicate the popen-read loop.
//
// Three entry points:
//   run_capture   — run a command, capture stdout (replaces the many inline
//                   popen loops in probe.cppm, xlings.cppm, pack.cppm, …)
//   run_with_env  — run a command with extra env vars (replaces scattered
//                   _putenv_s() calls on Windows)
//   shell_quote   — platform-aware shell quoting (delegates to mcpp.xlings::shq;
//                   kept here so new code imports mcpp.process, not mcpp.xlings)
//
// NOTE on Windows shell_quote:
//   Do NOT use shell_quote() for the FIRST token in a popen/system command
//   string on Windows — cmd.exe strips the leading double-quote pair and the
//   binary name becomes unrecognised.  Use the raw path string as the first
//   token and shell_quote() only for arguments.  See xlings.cppm::shq() for
//   the full rationale.

module;
#include <cstdio>
#include <cstdlib>
#if defined(_WIN32)
#include <stdlib.h>    // _putenv_s
#define popen  _popen
#define pclose _pclose
#endif

export module mcpp.process;

import std;
import mcpp.xlings;   // shq() — the authoritative shell-quoting implementation

export namespace mcpp::process {

// ─── Result type ─────────────────────────────────────────────────────────────

struct RunResult {
    int         exit_code = 0;
    std::string output;
};

// ─── run_capture ─────────────────────────────────────────────────────────────
//
// Run `command` via the platform shell (popen on both POSIX and Windows).
// Captures stdout.  stderr is NOT captured unless the caller redirects it
// in the command string (e.g. "cmd 2>&1" on POSIX, "cmd 2>&1" on Windows).
//
// Returns RunResult with exit_code set and output containing all captured
// text.  On popen failure, exit_code is -1 and output is empty.
RunResult run_capture(std::string_view command);

// ─── run_with_env ────────────────────────────────────────────────────────────
//
// Run `command` with extra environment variables (additive — existing vars
// not in `env` are preserved).
//
// On Windows: uses _putenv_s() to inject each var into the current process
// environment before spawning the child via popen().  _putenv_s() changes
// are inherited by child processes.  IMPORTANT: this mutates the calling
// process's environment; callers should restore vars if needed.
//
// On POSIX: prefixes the command with "VAR=val " tokens so the vars are
// scoped to the child (the calling process's environment is unchanged).
//
// Returns the same RunResult as run_capture().
RunResult run_with_env(std::string_view command,
                       const std::vector<std::pair<std::string, std::string>>& env);

// ─── shell_quote ─────────────────────────────────────────────────────────────
//
// Quote `s` for safe embedding in a shell command string.
//   POSIX:   wraps in single quotes, escaping embedded single quotes.
//   Windows: wraps in double quotes, escaping embedded double quotes.
//
// See the module-level NOTE about cmd.exe's first-token behaviour on Windows.
std::string shell_quote(std::string_view s);

} // namespace mcpp::process

// ─── Implementation ──────────────────────────────────────────────────────────

namespace mcpp::process {

RunResult run_capture(std::string_view command) {
    std::string cmd_str(command);
    RunResult result;

    std::FILE* fp = ::popen(cmd_str.c_str(), "r");
    if (!fp) {
        result.exit_code = -1;
        return result;
    }

    std::array<char, 4096> buf{};
    while (std::fgets(buf.data(), static_cast<int>(buf.size()), fp) != nullptr)
        result.output += buf.data();

    int rc = ::pclose(fp);
#if defined(_WIN32)
    // On Windows, pclose() returns the raw exit code from WaitForSingleObject /
    // GetExitCodeProcess — it is already the process exit code, not a wait
    // status word, so no WIFEXITED/WEXITSTATUS unwrapping needed.
    result.exit_code = rc;
#else
    // On POSIX, pclose() returns a wait-status word; extract the real exit code.
    if (WIFEXITED(rc))
        result.exit_code = WEXITSTATUS(rc);
    else
        result.exit_code = rc;   // signal / abnormal — surface raw value
#endif
    return result;
}

RunResult run_with_env(std::string_view command,
                       const std::vector<std::pair<std::string, std::string>>& env)
{
#if defined(_WIN32)
    // Inject vars into the current process environment.  popen() inherits them.
    for (auto& [k, v] : env)
        _putenv_s(k.c_str(), v.c_str());
    return run_capture(command);
#else
    // Build "KEY=val KEY2=val2 <original command>" prefix.
    std::string prefixed;
    for (auto& [k, v] : env) {
        prefixed += k;
        prefixed += '=';
        prefixed += shell_quote(v);
        prefixed += ' ';
    }
    prefixed += command;
    return run_capture(prefixed);
#endif
}

std::string shell_quote(std::string_view s) {
    // Delegate to the canonical implementation in mcpp.xlings so the two
    // stay in sync.  If xlings.cppm's shq() is ever updated for edge-cases
    // (e.g. NUL bytes, Unicode), this function inherits the fix automatically.
    return mcpp::xlings::shq(s);
}

} // namespace mcpp::process
