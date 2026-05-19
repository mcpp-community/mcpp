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

// Full silent redirect (stdin + stdout + stderr → /dev/null).
#if defined(_WIN32)
constexpr std::string_view silent_redirect = ">nul 2>&1";
#else
constexpr std::string_view silent_redirect = ">/dev/null 2>&1";
#endif

} // namespace mcpp::platform::shell

// ─── Implementation ──────────────────────────────────────────────────────

namespace mcpp::platform::shell {

std::string quote(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 2);
#if defined(_WIN32)
    out.push_back('"');
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else out.push_back(c);
    }
    out.push_back('"');
#else
    out.push_back('\'');
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out.push_back(c);
    }
    out.push_back('\'');
#endif
    return out;
}

} // namespace mcpp::platform::shell
