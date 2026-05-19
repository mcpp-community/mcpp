// mcpp.platform.env — platform-aware environment variable operations.
//
// Windows: uses _putenv_s to mutate the calling process environment.
// POSIX:   builds "KEY=val" shell prefix strings (no process mutation).

module;
#include <cstdlib>
#if defined(_WIN32)
#include <stdlib.h>    // _putenv_s
#endif

export module mcpp.platform.env;

import std;

export namespace mcpp::platform::env {

// Get an environment variable.  Returns nullopt if not set.
std::optional<std::string> get(std::string_view key);

// Set an environment variable in the current process.
// On POSIX this is a no-op by design — use build_env_prefix() instead
// to scope vars to a child process via command-line prefixing.
void set(const std::string& key, const std::string& value);

// Build a shell command prefix that injects the given env vars.
// Windows: calls set() for each var and returns "".
// POSIX:   returns "KEY1='val1' KEY2='val2' " (caller prepends to command).
std::string build_env_prefix(
    const std::vector<std::pair<std::string, std::string>>& vars);

} // namespace mcpp::platform::env

// ─── Implementation ──────────────────────────────────────────────────────

namespace mcpp::platform::env {

std::optional<std::string> get(std::string_view key) {
    std::string k(key);
    auto* v = std::getenv(k.c_str());
    if (!v || !*v) return std::nullopt;
    return std::string(v);
}

void set(const std::string& key, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(key.c_str(), value.c_str());
#else
    // POSIX: intentional no-op.  Use build_env_prefix() instead.
    (void)key;
    (void)value;
#endif
}

std::string build_env_prefix(
    const std::vector<std::pair<std::string, std::string>>& vars)
{
#if defined(_WIN32)
    for (auto& [k, v] : vars)
        _putenv_s(k.c_str(), v.c_str());
    return "";
#else
    std::string prefix;
    for (auto& [k, v] : vars) {
        prefix += k;
        prefix += '=';
        prefix += '\'';
        for (char c : v) {
            if (c == '\'') prefix += "'\\''";
            else prefix += c;
        }
        prefix += '\'';
        prefix += ' ';
    }
    return prefix;
#endif
}

} // namespace mcpp::platform::env
