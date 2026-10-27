// mcpp.effects: the knobs read through this invocation's environment.
module mcpp.effects;

import std;
import mcpp.invocation;

namespace mcpp::effects {

bool offline_mode() {
    // Not cached: `mcpp test` runs nested mcpp invocations in-process in some
    // paths, and a test that flips the var must see the change.
    const auto v = mcpp::invocation::get("MCPP_OFFLINE");
    return (v && *v != "0") || effect_denied("network");
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
