// mcpp.platform.env — platform-aware environment variable operations.
//
// Windows: uses _putenv_s to mutate the calling process environment.
// POSIX:   supports scoped current-process env mutation for child processes.

module;
#include <cstdlib>
#if defined(_WIN32)
#include <stdlib.h>    // _putenv_s
#else
#include <stdlib.h>    // setenv / unsetenv
#endif

export module mcpp.platform.env;

import std;

export namespace mcpp::platform::env {

// Get an environment variable.  Returns nullopt if not set.
std::optional<std::string> get(std::string_view key);

// Set an environment variable in the current process.
void set(const std::string& key, const std::string& value);

// Temporarily set or unset an env var, restoring the prior value on scope exit.
class ScopedEnv {
public:
    ScopedEnv(std::string key, std::optional<std::string> value);
    ~ScopedEnv();

    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    std::string key_;
    std::optional<std::string> previous_;
    bool had_previous_ = false;
};

std::string path_list_separator();
std::string runtime_library_path_key();
std::string prepend_path_list(std::string_view key,
                              std::span<const std::filesystem::path> dirs);

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
    setenv(key.c_str(), value.c_str(), 1);
#endif
}

ScopedEnv::ScopedEnv(std::string key, std::optional<std::string> value)
    : key_(std::move(key)) {
    if (auto* existing = std::getenv(key_.c_str())) {
        previous_ = std::string(existing);
        had_previous_ = true;
    }

    if (value) {
        set(key_, *value);
    } else {
#if defined(_WIN32)
        _putenv_s(key_.c_str(), "");
#else
        unsetenv(key_.c_str());
#endif
    }
}

ScopedEnv::~ScopedEnv() {
    if (had_previous_ && previous_) {
        set(key_, *previous_);
    } else {
#if defined(_WIN32)
        _putenv_s(key_.c_str(), "");
#else
        unsetenv(key_.c_str());
#endif
    }
}

std::string path_list_separator() {
#if defined(_WIN32)
    return ";";
#else
    return ":";
#endif
}

std::string runtime_library_path_key() {
#if defined(_WIN32)
    return "PATH";
#elif defined(__APPLE__)
    // DYLD_LIBRARY_PATH affects every executable launched by ninja, including
    // ninja itself, and can make macOS system frameworks load an incompatible
    // private libc++/libc++abi. Keep macOS on toolchain-provided rpaths.
    return "";
#elif defined(__linux__)
    return "LD_LIBRARY_PATH";
#else
    return "";
#endif
}

std::string prepend_path_list(std::string_view key,
                              std::span<const std::filesystem::path> dirs) {
    if (key.empty() || dirs.empty()) return "";

    auto sep = path_list_separator();
    std::string value;
    for (auto& dir : dirs) {
        if (dir.empty()) continue;
        if (!value.empty()) value += sep;
        value += dir.string();
    }
    if (value.empty()) return "";

    std::string k(key);
    if (auto* existing = std::getenv(k.c_str()); existing && *existing) {
        value += sep;
        value += existing;
    }
    return value;
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
