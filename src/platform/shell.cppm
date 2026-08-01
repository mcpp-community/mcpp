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

std::string quote(std::string_view s) {
#if defined(_WIN32)
    return quote_windows(s);
#else
    return quote_posix(s);
#endif
}

} // namespace mcpp::platform::shell
