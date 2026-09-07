// mcpp.platform.shell — platform-aware shell quoting and redirect helpers.
//
// Provides shell-safe argument quoting for command construction:
//   POSIX:   single-quote wrapping  ('arg')
//   Windows: double-quote wrapping  ("arg")
//
// NOTE on Windows: do NOT use quote() for the FIRST token in a
// popen/system command string — cmd.exe strips a leading " pair.
// Use the raw path string as the first token; quote() is safe for
// arguments only.

module;

export module mcpp.platform.shell;

import std;

export namespace mcpp::platform::shell {

// Platform-aware shell argument quoting.
std::string quote(std::string_view s);

// The two halves of `quote`, callable regardless of host. Exposed so the
// Windows command-line shape can be built and unit-tested from any platform
// — the cmd.exe quoting rules are the easiest thing in mcpp to get wrong and
// the hardest to notice, since a Linux/macOS run never executes that code.
std::string quote_windows(std::string_view s);
std::string quote_posix(std::string_view s);

// An argument that must survive TWO parsers: cmd.exe's, and then the child
// program's own argv parsing.
//
// `quote_windows` answers only the second. Its `\"` escape belongs to the
// MSVCRT argv rules, and cmd.exe does not know it: to cmd every `"` simply
// toggles a quote state, so a payload carrying an EVEN number of them before a
// metacharacter leaves that character unquoted. Measured on windows-2022 with
// a JSON argument -- `{"targets":["xim:shaderc@>=2026.3"],"yes":true}` puts
// four quotes before the `>`, and cmd read it as a REDIRECTION, answering
//
//   The filename, directory name, or volume label syntax is incorrect.
//
// which arrived as a package-provisioning failure naming a package.
//
// The answer is the standard double escape: quote for the child, then prefix
// every cmd metacharacter -- the quotes included -- with `^`. With no `"` left
// unescaped cmd never enters a quoted region, so every metacharacter is
// escaped rather than quoted, which is the only state in which both rules
// hold. cmd removes the carets and the child sees exactly `quote_windows`.
//
// `%` IS NOT ESCAPED AND CANNOT BE. Variable expansion happens before caret
// processing, and the batch-file escape (`%%`) is not available on a command
// line. Callers passing text that may contain `%` need a different mechanism;
// the JSON arguments this exists for do not.
std::string quote_windows_through_cmd(std::string_view s);

// Host-selecting: `quote_windows_through_cmd` on Windows, `quote_posix`
// elsewhere. Use this wherever an argument reaches a shell and may contain
// metacharacters -- notably JSON, and any version constraint spelled `>=`.
std::string quote_through_shell(std::string_view s);

// Silent redirect — stdout + stderr → /dev/null (or NUL on Windows).
// stdin is NOT touched here; that's the responsibility of
// mcpp::platform::process::seal_stdin, which is auto-applied by capture /
// run_silent / run_streaming on all platforms.
#if defined(_WIN32)
constexpr std::string_view silent_redirect = ">nul 2>&1";
#else
constexpr std::string_view silent_redirect = ">/dev/null 2>&1";
#endif

} // namespace mcpp::platform::shell

// ─── Implementation ──────────────────────────────────────────────────────

namespace mcpp::platform::shell {

std::string quote_windows(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('"');
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else out.push_back(c);
    }
    out.push_back('"');
    return out;
}

std::string quote_posix(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('\'');
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out.push_back(c);
    }
    out.push_back('\'');
    return out;
}

std::string quote_windows_through_cmd(std::string_view s) {
    const std::string inner = quote_windows(s);
    std::string out;
    out.reserve(inner.size() * 2);
    for (char c : inner) {
        switch (c) {
            case '"': case '<': case '>': case '&': case '|':
            case '^': case '(': case ')':
                out.push_back('^');
                break;
            default:
                break;
        }
        out.push_back(c);
    }
    return out;
}

std::string quote(std::string_view s) {
#if defined(_WIN32)
    return quote_windows(s);
#else
    return quote_posix(s);
#endif
}

std::string quote_through_shell(std::string_view s) {
#if defined(_WIN32)
    return quote_windows_through_cmd(s);
#else
    return quote_posix(s);
#endif
}

} // namespace mcpp::platform::shell
