// mcpp.effects: the knobs read through this invocation's environment.
module mcpp.effects;

import std;
import mcpp.invocation;
import mcpplibs.os.env;

namespace mcpp::effects {

bool offline_mode() {
    // Not cached: `mcpp test` runs nested mcpp invocations in-process in some
    // paths, and a test that flips the var must see the change.
    const auto v = mcpp::invocation::get("MCPP_OFFLINE");
    return (v && *v != "0") || effect_denied("network");
}

bool host_tool_allowed(std::string_view tool) {
    const auto v = mcpp::invocation::get("MCPP_ALLOW_HOST_TOOLS");
    if (!v) return false;
    for (auto part : std::views::split(std::string_view(*v), ',')) {
        std::string_view name(part.begin(), part.end());
        while (!name.empty() && name.front() == ' ') name.remove_prefix(1);
        while (!name.empty() && name.back() == ' ') name.remove_suffix(1);
        if (name == tool) return true;
    }
    return false;
}

const std::vector<std::string_view>& deniable_effects() {
    static const std::vector<std::string_view> names{
        "init-mcpp-home", "write-project", "write-global-cache", "network", "exec-build-script"};
    return names;
}

bool effect_denied(std::string_view effect) {
    const auto v = mcpp::invocation::get("MCPP_DENY_EFFECTS");
    if (!v) return false;
    std::string_view list(*v);
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

namespace {
struct Statement { std::string value; std::string where; };
std::mutex g_keysMutex;
// Each variable's statements by tier; the variable holds the highest's.
std::map<std::string, std::map<Tier, Statement>, std::less<>> g_keys;

// The caller holds g_keysMutex. A statement holds while the variable keeps
// its value: one the caller set, or a nested run's, is the caller's.
KeyOrigin origin_locked(std::string_view var) {
    const auto v = mcpp::invocation::get(var);
    if (!v) return {};
    if (auto it = g_keys.find(var); it != g_keys.end() && !it->second.empty()
                                    && it->second.rbegin()->second.value == *v)
        return {it->second.rbegin()->first, it->second.rbegin()->second.where};
    return {Tier::Override, std::string(var)};
}

void apply_locked(std::string_view var) {
    auto it = g_keys.find(var);
    if (it == g_keys.end() || it->second.empty()) mcpp::invocation::unset(std::string(var));
    else mcpp::invocation::set(std::string(var), it->second.rbegin()->second.value);
}
}  // namespace

void state_key(std::string_view var, Tier tier, std::string value, std::string where) {
    std::lock_guard lock(g_keysMutex);
    if (origin_locked(var).tier == Tier::Override) return;
    g_keys[std::string(var)].insert_or_assign(tier, Statement{std::move(value), std::move(where)});
    apply_locked(var);
}

void withdraw_key(std::string_view var, Tier tier) {
    std::lock_guard lock(g_keysMutex);
    if (origin_locked(var).tier == Tier::Override) return;
    if (auto it = g_keys.find(var); it != g_keys.end() && it->second.erase(tier)) apply_locked(var);
}

KeyOrigin key_origin(std::string_view var) {
    std::lock_guard lock(g_keysMutex);
    return origin_locked(var);
}

std::string offline_remedy() {
    if (effect_denied("network") && !mcpp::invocation::get("MCPP_OFFLINE"))
        return "drop `network` from --deny (MCPP_DENY_EFFECTS)";
    const auto origin = key_origin("MCPP_OFFLINE");
    switch (origin.tier) {
        case Tier::Project: return "set `offline = false` in mcpp.toml [build], or run with MCPP_OFFLINE=0";
        case Tier::Machine: return "set `offline = false` in config.toml [build], or run with MCPP_OFFLINE=0";
        default:            return "run without --offline (or unset MCPP_OFFLINE)";
    }
}

std::vector<std::filesystem::path> archive_dirs() {
    const auto v = mcpp::invocation::get("MCPP_ARCHIVES");
    return v ? mcpplibs::os::env::split_list(*v) : std::vector<std::filesystem::path>{};
}

InstallSources install_sources() {
    if (!offline_mode()) return InstallSources::Network;
    return archive_dirs().empty() ? InstallSources::None : InstallSources::Local;
}

namespace {
std::mutex g_inputsMutex;
std::vector<std::string> g_packagesUsed;
std::vector<GitInput> g_gitUsed;
}

void note_package_used(std::string ref) {
    std::lock_guard lock(g_inputsMutex);
    if (std::ranges::find(g_packagesUsed, ref) == g_packagesUsed.end()) g_packagesUsed.push_back(std::move(ref));
}

void note_git_used(GitInput input) {
    std::lock_guard lock(g_inputsMutex);
    if (std::ranges::none_of(g_gitUsed, [&](const GitInput& g) { return g.clone == input.clone; }))
        g_gitUsed.push_back(std::move(input));
}

std::vector<std::string> packages_used() {
    std::lock_guard lock(g_inputsMutex);
    return g_packagesUsed;
}

std::vector<GitInput> git_used() {
    std::lock_guard lock(g_inputsMutex);
    return g_gitUsed;
}

bool no_auto_install() {
    const auto v = mcpp::invocation::get("MCPP_NO_AUTO_INSTALL");
    return v && *v != "0";
}

namespace {
std::atomic<bool> g_network_accessed{false};
}

void note_network_access() { g_network_accessed.store(true, std::memory_order_relaxed); }
bool network_accessed()    { return g_network_accessed.load(std::memory_order_relaxed); }

}  // namespace mcpp::effects
