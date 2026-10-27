// mcpp.invocation over mcpplibs.os: the started environment read through
// os::env, the statements held here.
module mcpp.invocation;

import std;
import mcpplibs.os.platform;
import mcpplibs.os.env;
import mcpplibs.os.process;

namespace mcpp::invocation {

namespace {

namespace os = mcpplibs::os;

// Names compare without case on Windows, as the system compares them.
std::string key_of(std::string_view name) {
    std::string k(name);
    if constexpr (os::is_windows)
        for (auto& c : k) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return k;
}

struct Statement {
    std::string name;                  // as stated
    std::optional<std::string> value;  // nullopt: absent
};

std::shared_mutex g_lock;
std::map<std::string, Statement>& statements() {
    static std::map<std::string, Statement> s;
    return s;
}

std::optional<std::optional<std::string>> stated(std::string_view name) {
    std::shared_lock g(g_lock);
    auto& s = statements();
    if (auto it = s.find(key_of(name)); it != s.end()) return it->second.value;
    return std::nullopt;
}

}  // namespace

std::optional<std::string> get(std::string_view name) {
    if (auto s = stated(name)) {
        if (!*s || (*s)->empty()) return std::nullopt;
        return **s;
    }
    auto v = os::env::get(name);
    if (!v || v->empty()) return std::nullopt;
    return v;
}

void set(std::string name, std::string value) {
    std::unique_lock g(g_lock);
    auto k = key_of(name);
    statements()[k] = Statement{ std::move(name), std::move(value) };
}

void unset(std::string name) {
    std::unique_lock g(g_lock);
    auto k = key_of(name);
    statements()[k] = Statement{ std::move(name), std::nullopt };
}

std::vector<std::string> with_prefix(std::string_view prefix) {
    std::map<std::string, std::string> seen;   // key -> NAME=value
    for (auto& e : os::env::variables()) {
        const auto eq = e.find('=');
        if (eq == std::string::npos || eq + 1 == e.size()) continue;
        if (!std::string_view(e).starts_with(prefix)) continue;
        seen[key_of(std::string_view(e).substr(0, eq))] = e;
    }
    {
        std::shared_lock g(g_lock);
        for (auto& [k, st] : statements()) {
            if (!std::string_view(st.name).starts_with(prefix)) continue;
            if (!st.value || st.value->empty()) seen.erase(k);
            else seen[k] = st.name + "=" + *st.value;
        }
    }
    std::vector<std::string> out;
    for (auto& [k, e] : seen) out.push_back(e);
    std::ranges::sort(out);
    return out;
}

std::vector<std::pair<std::string, std::optional<std::string>>> overrides() {
    std::shared_lock g(g_lock);
    std::vector<std::pair<std::string, std::optional<std::string>>> out;
    for (auto& [k, st] : statements()) out.emplace_back(st.name, st.value);
    return out;
}

ScopedEnv::ScopedEnv(std::string name, std::optional<std::string> value) : name_(std::move(name)) {
    previous_ = stated(name_);
    if (value) set(name_, *value);
    else unset(name_);
}

ScopedEnv::~ScopedEnv() {
    std::unique_lock g(g_lock);
    auto& s = statements();
    if (!previous_) s.erase(key_of(name_));
    else s[key_of(name_)] = Statement{ name_, *previous_ };
}

std::optional<std::filesystem::path> env_directory(std::string_view name) {
    if (auto v = get(name)) return std::filesystem::path(*v);
    return std::nullopt;
}

void prepend_path(const std::filesystem::path& dir) {
    const auto current = get("PATH").value_or("");
    const std::string sep(1, os::path_list_separator);
    set("PATH", current.empty() ? dir.string() : dir.string() + sep + current);
}

std::optional<std::filesystem::path> which(std::string_view program) {
    return os::env::find_executable(program, get("PATH").value_or(""));
}

std::filesystem::path self_exe_path() {
    return os::env::image().value_or(std::filesystem::path{});
}

long process_id() {
    static const long id = [] {
        auto me = os::process::self();
        return me ? static_cast<long>(me->id) : 0L;
    }();
    return id;
}

}  // namespace mcpp::invocation
