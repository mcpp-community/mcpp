// mcpp.toolchain.target_rows — the rows and providers a process knows.
//
// The built-in table (data/builtin.toml) and the definitions of the platforms a
// build has read (`[[platform.define]]`, `[[platform.provider]]`; SPEC-016 §2,
// §3). A platform's rows extend the vocabulary for every later question in the
// process: the triple parser accepts the os and env a defined row names, and
// `cfg(os = ...)` evaluates against the row. A definition stays for the process,
// because one process plans one build and the configurations it derives from it.

export module mcpp.toolchain.target_rows;

import std;
import mcpp.toolchain.target_schema;
import mcpp.toolchain.builtin_targets;

export namespace mcpp::toolchain::target {

// THE STRINGS A DEFINITION OWNS. A row and a provider view strings; a defined
// one views the strings of the set that read it, which lives as long as the
// process does once it is defined.
struct DefinitionSet {
    std::string source;                         // the platform package that stated it
    // A row assumed for a request no definition knows yet, while the platform
    // that may define it is fetched; a definition of the same row replaces it.
    bool provisional = false;
    std::deque<std::string> strings;            // stable addresses
    std::deque<std::vector<std::string_view>> lists;
    std::vector<Row> rows;
    std::vector<Provider> providers;
    std::vector<System> systems;

    std::string_view keep(std::string s) { return strings.emplace_back(std::move(s)); }
    std::span<const std::string_view> keep(const std::vector<std::string>& v) {
        auto& out = lists.emplace_back();
        for (auto const& s : v) out.push_back(keep(s));
        return out;
    }
};

// Registers a platform's definitions. A row the table or an earlier set already
// defines is not defined again; the answer is the first definition's.
void define(DefinitionSet set);

// Every row: the built-in ones in the table's order, then the defined ones.
std::vector<const Row*> rows();
std::vector<const Provider*> providers();
const Row* find_row(std::string_view triple);

// The system table: the built-in systems, then those the platforms state. A
// system stated twice is the first statement's.
std::vector<const System*> systems();
const System* find_system(std::string_view name);

// Whether a defined row names this os, or this env under this os: the words the
// parser accepts beyond its own.
bool defines_os(std::string_view os);
bool defines_env(std::string_view os, std::string_view env);

// The row a platform defined for this triple; null when none did (the cheap
// answer while nothing is defined, which is every build without such a platform).
const Row* defined_row(std::string_view triple);

} // namespace mcpp::toolchain::target

namespace mcpp::toolchain::target {

namespace {

struct Registry {
    std::mutex lock;
    std::deque<DefinitionSet> sets;   // stable addresses
};

std::atomic<bool> g_anyDefined{false};

Registry& registry() {
    static Registry r;
    return r;
}

} // namespace

void define(DefinitionSet set) {
    auto& r = registry();
    std::lock_guard g(r.lock);
    // A source defines once per process; a provisional row gives way to a real one.
    for (auto const& s : r.sets)
        if (!set.source.empty() && s.source == set.source && !s.provisional && !set.provisional) return;
    if (!set.provisional)
        for (auto& s : r.sets)
            if (s.provisional)
                std::erase_if(s.rows, [&](const Row& old) {
                    return std::ranges::any_of(set.rows, [&](const Row& n) { return n.triple == old.triple; });
                });
    std::erase_if(set.rows, [&](const Row& row) {
        for (auto const& b : mcpp::toolchain::builtin::kRows)
            if (b.triple == row.triple) return true;
        for (auto const& s : r.sets)
            for (auto const& d : s.rows)
                if (d.triple == row.triple) return true;
        return false;
    });
    r.sets.push_back(std::move(set));
    g_anyDefined = true;
}

const Row* defined_row(std::string_view triple) {
    if (!g_anyDefined) return nullptr;
    auto& r = registry();
    std::lock_guard g(r.lock);
    for (auto const& s : r.sets)
        for (auto const& d : s.rows)
            if (d.triple == triple) return &d;
    return nullptr;
}

std::vector<const Row*> rows() {
    std::vector<const Row*> out;
    for (auto const& b : mcpp::toolchain::builtin::kRows) out.push_back(&b);
    auto& r = registry();
    std::lock_guard g(r.lock);
    for (auto const& s : r.sets)
        for (auto const& d : s.rows) out.push_back(&d);
    return out;
}

std::vector<const Provider*> providers() {
    std::vector<const Provider*> out;
    for (auto const& b : mcpp::toolchain::builtin::kProviders) out.push_back(&b);
    auto& r = registry();
    std::lock_guard g(r.lock);
    for (auto const& s : r.sets)
        for (auto const& p : s.providers) out.push_back(&p);
    return out;
}

const Row* find_row(std::string_view triple) {
    for (auto const* r : rows())
        if (r->triple == triple) return r;
    return nullptr;
}

std::vector<const System*> systems() {
    std::vector<const System*> out;
    for (auto const& b : mcpp::toolchain::builtin::kSystems) out.push_back(&b);
    auto& r = registry();
    std::lock_guard g(r.lock);
    for (auto const& s : r.sets)
        for (auto const& d : s.systems)
            if (std::ranges::none_of(out, [&](const System* k) { return k->name == d.name; }))
                out.push_back(&d);
    return out;
}

const System* find_system(std::string_view name) {
    for (auto const* s : systems())
        if (s->name == name) return s;
    return nullptr;
}

bool defines_os(std::string_view os) {
    // The built-in rows' systems are words of the language too (design
    // 2026-11-10 E4): a system the data names -- FreeBSD -- needs no word in
    // the parser's code.
    for (auto const& b : mcpp::toolchain::builtin::kRows)
        if (b.os == os) return true;
    auto& r = registry();
    std::lock_guard g(r.lock);
    for (auto const& s : r.sets)
        for (auto const& d : s.rows)
            if (d.os == os) return true;
    return false;
}

bool defines_env(std::string_view os, std::string_view env) {
    for (auto const& b : mcpp::toolchain::builtin::kRows)
        if (b.os == os && b.env == env) return true;
    auto& r = registry();
    std::lock_guard g(r.lock);
    for (auto const& s : r.sets)
        for (auto const& d : s.rows)
            if (d.os == os && d.env == env) return true;
    return false;
}

} // namespace mcpp::toolchain::target
