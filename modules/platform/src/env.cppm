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
extern char** environ;
#endif

export module mcpp.platform.env;

import std;

export namespace mcpp::platform::env {

// Get an environment variable.  Returns nullopt if not set.
std::optional<std::string> get(std::string_view key);

// Set an environment variable in the current process.
void set(const std::string& key, const std::string& value);

// Remove an environment variable from the current process. On Windows this is
// `_putenv_s(key, "")`, which the CRT defines as removal, so absent and empty
// are one state there, and `get` answers nullopt for both on every platform.
void unset(const std::string& key);

// Every variable whose name starts with `prefix`, as `NAME=value` in the
// order the process environment holds them, empty values left out. For an
// identity that has to change when any of a family of variables does
// (`MCPP_TARGET_<TRIPLE>_SYSROOT`), where the names are not known in advance.
std::vector<std::string> with_prefix(std::string_view prefix);

// ── Network policy: offline mode ──────────────────────────────────────────
//
// One process-wide knob answering "may mcpp reach the network at all?", read
// at each point of use rather than threaded through a dozen signatures — the
// same idiom the older MCPP_NO_AUTO_INSTALL gate already uses.
//
// It lives HERE, in a leaf module, on purpose: the consumers span pm (index
// refresh, package install) and build (toolchain auto-install), and any home
// inside one of those subsystems would make the other import it across a layer
// boundary — mcpp.pm.index_route already imports mcpp.fetcher, so a pm-owned
// helper could not be read from the fetcher without a module cycle.
//
// Set by `--offline` (cli pre-scan writes the env var) or by the user exporting
// MCPP_OFFLINE. "0" and the empty string mean off, matching MCPP_VERBOSE.
bool offline_mode();

// THE EFFECTS THIS RUN MAY NOT HAVE (design 2026-10-26 §5.5, D8).
//
// `--deny <effect,…>` and MCPP_DENY_EFFECTS name effects of docs/50 §4 the
// caller does not allow: `network`, `write-project`, `write-global-cache`,
// `exec-build-script`, `init-mcpp-home` (`read-project` cannot be denied).
// The two are a union, and nothing a project declares widens them: the
// constraint is the caller's. `--offline` is `--deny network`, and both
// spellings answer `offline_mode()`. A run that stops before a denied effect
// records it here, and the process exits 5 (SPEC-003): a caller tells "my
// constraint stopped it" from "it failed" by the status alone.
bool effect_denied(std::string_view effect);
// The deniable effects, for the CLI's validation of `--deny`.
const std::vector<std::string_view>& deniable_effects();
void note_effect_refused(std::string_view effect, std::string_view what);
// "network: install xim:gcc@15.1.0" -- the first refusal of this run, if any.
std::optional<std::string> effect_refused();

// WHETHER THIS RUN WRITES THE GLOBAL CACHE. Answers kept under the home (a
// compiler's probe, the provider's handshake) are read by every command and
// written only by a run whose effects include `write-global-cache`: one that
// plans or builds (it says so with `note_writes_global_cache`) and was not
// denied it. A query -- `self doctor`, `self env`, `toolchain list` -- declares
// no such effect, so it leaves the home exactly as it found it.
void note_writes_global_cache();
bool writes_global_cache();

// The older, narrower spelling: MCPP_NO_AUTO_INSTALL gates ONLY the toolchain
// auto-install, nothing else. Kept working (CI and existing setups export it)
// and kept next to offline_mode() so the two network knobs cannot drift apart.
bool no_auto_install();

// WHAT THIS RUN DID WITH THE NETWORK, AS OPPOSED TO WHAT IT MAY DO.
//
// docs/50 defines an envelope's `effects` as what running the command did,
// and `--protocol-version` as what a command may do. The per-run list used to
// be assembled from constants, so a plan that refreshed the index reported no
// `network` (#648 A4). A launcher of a network-bound child (an index refresh,
// an install, a git remote operation) calls `note_network_access` before it
// starts the child; an attempt that fails or times out still counts, because
// the effect is the access. Offline, those launchers return before this call.
// Kept beside offline_mode() for the reason stated above: every layer can reach
// this module, and the record has writers in several of them.
void note_network_access();
bool network_accessed();

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
// The PATH this process was started with. On Windows a child's environment is
// applied to this process (`_putenv_s`), so `getenv("PATH")` answers what the
// children launched so far were given; a value that must not depend on that --
// an environment a build program's run is keyed on -- starts from this one.
const std::string& inherited_path();
std::string runtime_library_path_key();
std::string host_tool_runtime_library_path_key();

// Drop mcpp's private-glibc payload entries from a loader search path.
//
// An outer `mcpp run`/`mcpp test` points LD_LIBRARY_PATH at the payload so ITS
// child (a sandbox-linked binary) can load. Anything further down the process
// tree that is a HOST binary — /bin/sh, or a payload tool patched against a
// DIFFERENT payload version — then resolves a mismatched libc.so.6 against the
// host ld.so and dies inside the dynamic linker before main (signature: a bare
// `__vdso_time` line, then SIGSEGV). libc and ld.so are version-locked through
// GLIBC_PRIVATE, so this never reproduces when the two happen to match.
//
// Lives here, next to path-list composition, because BOTH consumers need the
// same predicate: process.cppm sanitizes the INHERITED variable for every child,
// and prepend_path_list sanitizes the inherited TAIL it carries into a composed
// override (dirs the caller passed explicitly are always kept — that is the
// entry the sandbox binary actually needs).
std::string strip_private_glibc(std::string_view paths);

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

namespace {
// Initialised before main, ahead of anything that starts a child.
const std::string kInheritedPath = [] {
    const char* p = std::getenv("PATH");
    return p ? std::string(p) : std::string{};
}();
}  // namespace

const std::string& inherited_path() { return kInheritedPath; }

bool offline_mode() {
    // Not cached: `mcpp test` runs nested mcpp invocations in-process in some
    // paths, and a test that flips the var must see the change.
    auto* v = std::getenv("MCPP_OFFLINE");
    return (v && *v && std::string_view(v) != "0") || effect_denied("network");
}

const std::vector<std::string_view>& deniable_effects() {
    static const std::vector<std::string_view> names{
        "init-mcpp-home", "write-project", "write-global-cache", "network", "exec-build-script"};
    return names;
}

bool effect_denied(std::string_view effect) {
    auto* v = std::getenv("MCPP_DENY_EFFECTS");
    if (!v || !*v) return false;
    std::string_view list(v);
    while (!list.empty()) {
        auto comma = list.find(',');
        auto item = list.substr(0, comma);
        while (!item.empty() && item.front() == ' ') item.remove_prefix(1);
        while (!item.empty() && item.back() == ' ') item.remove_suffix(1);
        if (item == effect) return true;
        if (comma == std::string_view::npos) break;
        list.remove_prefix(comma + 1);
    }
    return false;
}

namespace {
std::mutex g_refusedMutex;
std::optional<std::string> g_refused;
}

void note_effect_refused(std::string_view effect, std::string_view what) {
    std::lock_guard lock(g_refusedMutex);
    if (!g_refused) g_refused = what.empty() ? std::string(effect)
                                             : std::format("{}: {}", effect, what);
}

std::optional<std::string> effect_refused() {
    std::lock_guard lock(g_refusedMutex);
    return g_refused;
}

namespace {
std::atomic<bool> g_writesGlobalCache{false};
}

void note_writes_global_cache() { g_writesGlobalCache.store(true); }

bool writes_global_cache() {
    return g_writesGlobalCache.load() && !effect_denied("write-global-cache");
}

bool no_auto_install() {
    auto* v = std::getenv("MCPP_NO_AUTO_INSTALL");
    return v && *v && std::string_view(v) != "0";
}

namespace {
std::atomic<bool> g_network_accessed{false};
}

void note_network_access() { g_network_accessed.store(true, std::memory_order_relaxed); }
bool network_accessed()    { return g_network_accessed.load(std::memory_order_relaxed); }

std::optional<std::string> get(std::string_view key) {
    std::string k(key);
    auto* v = std::getenv(k.c_str());
    if (!v || !*v) return std::nullopt;
    return std::string(v);
}

std::vector<std::string> with_prefix(std::string_view prefix) {
    std::vector<std::string> out;
#if defined(_WIN32)
    char** env = _environ;
#else
    char** env = environ;
#endif
    for (; env && *env; ++env) {
        std::string_view e(*env);
        if (!e.starts_with(prefix)) continue;
        auto eq = e.find('=');
        if (eq == std::string_view::npos || eq + 1 == e.size()) continue;
        out.emplace_back(e);
    }
    std::ranges::sort(out);
    return out;
}

void set(const std::string& key, const std::string& value) {
#if defined(_WIN32)
    _putenv_s(key.c_str(), value.c_str());
#else
    setenv(key.c_str(), value.c_str(), 1);
#endif
}

void unset(const std::string& key) {
#if defined(_WIN32)
    _putenv_s(key.c_str(), "");
#else
    unsetenv(key.c_str());
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
        unset(key_);
    }
}

ScopedEnv::~ScopedEnv() {
    if (had_previous_ && previous_) {
        set(key_, *previous_);
    } else {
        unset(key_);
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

std::string host_tool_runtime_library_path_key() {
#if defined(__APPLE__)
    return "DYLD_LIBRARY_PATH";
#elif defined(__linux__)
    return "LD_LIBRARY_PATH";
#else
    return "";
#endif
}

std::string strip_private_glibc(std::string_view paths) {
    auto sep = path_list_separator();
    std::string cleaned;
    std::size_t start = 0;
    while (start <= paths.size()) {
        auto end = paths.find(sep, start);
        if (end == std::string_view::npos) end = paths.size();
        auto item = paths.substr(start, end - start);
        if (!item.empty() && item.find("/xim-x-glibc/") == std::string_view::npos
                          && item.find("\\xim-x-glibc\\") == std::string_view::npos) {
            if (!cleaned.empty()) cleaned += sep;
            cleaned += item;
        }
        if (end == paths.size()) break;
        start = end + sep.size();
    }
    return cleaned;
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
        // Loader paths only: the inherited value may carry an OUTER mcpp's
        // private-glibc entry, and the composed string becomes an EXPLICIT
        // override — which process.cppm's merged_environ takes verbatim,
        // skipping the sanitation it applies to inherited variables. Without
        // this the strip is defeated exactly when it matters (a nested
        // `mcpp run` → `mcpp test` chain), and the child tool segfaults in the
        // dynamic linker whenever the payload versions differ.
        std::string tail = (k == "LD_LIBRARY_PATH" || k == "DYLD_LIBRARY_PATH")
            ? strip_private_glibc(existing) : std::string(existing);
        if (!tail.empty()) {
            value += sep;
            value += tail;
        }
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
