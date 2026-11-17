// mcpp.buildopt.advice — what a build says of the rebuild it just made
// (SPEC-019 §6).
//
// Not an entry of the register: nothing here changes what is built. It reads
// what the build already holds -- which BMIs a run replaced (keep's record),
// which units import which modules (the plan) -- and says the one thing a
// developer can act on.
//
// ONE ADVICE: A MODULE THAT IS ONE FILE. Its function bodies are compiled with
// its interface, so an edit of one replaces the BMI where the compiler records
// it (clang: any byte of the source; gcc: a line moved between bodies) and
// every importer follows. The answer is in the language and in the register:
// bodies after `module :private;` are built as a unit of their own
// (`private-fragment`), or they go in an implementation unit. Said for the
// module the rebuild started at, when the importers that followed are many,
// and once a build directory: the developer who chose the shape is not told
// on every edit.

export module mcpp.buildopt.advice;

import std;

export namespace mcpp::buildopt::advice {

// A unit as the plan holds it, reduced to what the advice reads.
struct Unit {
    std::string              source;          // the file's name
    std::string              module;          // the module it provides; empty: none
    std::vector<std::string> imports;         // the modules it imports or implements
    bool                     implementation = false;   // `module M;`
};

struct Cascade {
    std::string module;
    std::string source;
    std::size_t importers = 0;   // units that import the module, directly or through others
};

inline constexpr std::size_t kManyImporters = 5;

// The modules a run's rebuild started at -- replaced, and importing no other
// replaced module -- that are one file with no implementation unit and at
// least `many` importing units. `replaced` holds BMI file names as the run
// recorded them (`a.b-part.pcm`).
std::vector<Cascade> single_file_cascades(const std::vector<Unit>& units,
                                          const std::vector<std::string>& replaced,
                                          std::size_t many = kManyImporters);

// The line a cascade is said with.
std::string line(const Cascade& c);

// Whether `module`'s advice has not been said in the build directory `dir`
// yet; saying it is recorded there (`buildopt-advice.list`).
bool first_time(const std::filesystem::path& dir, std::string_view module);

} // namespace mcpp::buildopt::advice

namespace mcpp::buildopt::advice {

std::vector<Cascade> single_file_cascades(const std::vector<Unit>& units,
                                          const std::vector<std::string>& replaced,
                                          std::size_t many) {
    auto bmi_stem = [](std::string name) {
        std::ranges::replace(name, ':', '-');
        return name;
    };
    std::set<std::string> replacedStems;
    for (auto const& r : replaced) replacedStems.insert(std::filesystem::path(r).stem().string());
    std::map<std::string, const Unit*> provider;
    for (auto const& u : units)
        if (!u.module.empty()) provider[u.module] = &u;
    auto was_replaced = [&](const std::string& module) { return replacedStems.contains(bmi_stem(module)); };

    std::vector<Cascade> out;
    for (auto const& [module, unit] : provider) {
        if (!was_replaced(module) || module.find(':') != std::string::npos) continue;
        // Where the rebuild started: no module this one imports was replaced.
        if (std::ranges::any_of(unit->imports, was_replaced)) continue;
        // One file: no implementation unit, no partition of it.
        const bool more = std::ranges::any_of(units, [&](const Unit& u) {
            return (u.implementation && std::ranges::contains(u.imports, module))
                || u.module.starts_with(module + ":");
        });
        if (more) continue;
        // The units that follow: importers of the module and of what imports it.
        std::set<std::string> reached{module};
        std::size_t importers = 0;
        std::set<const Unit*> counted;
        for (bool grew = true; grew;) {
            grew = false;
            for (auto const& u : units) {
                if (counted.contains(&u) || &u == unit) continue;
                if (!std::ranges::any_of(u.imports, [&](const std::string& i) { return reached.contains(i); })) continue;
                counted.insert(&u);
                ++importers;
                if (!u.module.empty() && reached.insert(u.module).second) grew = true;
                grew = true;
            }
        }
        if (importers >= many) out.push_back({module, unit->source, importers});
    }
    return out;
}

std::string line(const Cascade& c) {
    return std::format("{} is the whole of module {}, which {} units import: its function bodies are compiled "
                       "with its interface, so an edit of one can compile all {} again. After `module :private;` "
                       "they are a unit of their own, and an edit of them compiles that unit alone",
                       c.source, c.module, c.importers, c.importers);
}

bool first_time(const std::filesystem::path& dir, std::string_view module) {
    const auto file = dir / "buildopt-advice.list";
    {
        std::ifstream in(file, std::ios::binary);
        for (std::string said; std::getline(in, said);)
            if (said == module) return false;
    }
    std::ofstream out(file, std::ios::app | std::ios::binary);
    out << module << "\n";
    return true;
}

} // namespace mcpp::buildopt::advice
