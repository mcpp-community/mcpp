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
//
// A SECOND ADVICE: WHEN A BUILD FAILS AT A STEP AN ENTRY ACTED ON (SPEC-019
// §9). Most projects are right with every entry on; the few that are not need
// to learn, from the failure itself, how to find out whether an entry is the
// cause, without it being said on every failure. It reads the step that
// failed, not the entries that ran: a source built as two units that failed
// (R1), a unit that imports a BMI kept with the lines of an earlier source
// that failed (R2), a link that failed where either is in the graph (R3). Any
// other failure says nothing (R4). The text is made of the register's own key,
// environment name and `check` value: there is no second spelling of a switch.

export module mcpp.buildopt.advice;

import std;
import mcpp.buildopt.registry;

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

// WHAT THE PLAN HOLDS THAT A FAILURE READS (SPEC-019 §9), kept beside
// build.ninja (`buildopt-applied.tsv`) because the fast path, which has no
// plan, reports a failure as well.
struct Fragment {
    std::string              written;   // the source as it is written
    std::vector<std::string> units;     // the files it is built as
};
struct Importer {
    std::string              source;    // a unit that imports a module
    std::vector<std::string> imports;   // the modules it imports
};
struct Applied {
    std::vector<Fragment> fragments;
    std::vector<Importer> importers;
    std::string           fragmentCheck;   // the `private_fragment` value that checks, for this compiler
};
inline constexpr std::string_view kAppliedFile = "buildopt-applied.tsv";
// Written when it differs from what is there.
void write_applied(const std::filesystem::path& dir, const Applied& applied);
Applied read_applied(const std::filesystem::path& dir);

// The step that failed: the first input of each failed step (the source, for a
// compile), and whether the linker said a link failed.
struct Failed {
    std::vector<std::string> sources;
    bool                     linked = false;
};
// The outputs each `FAILED:` line of ninja's output names.
std::vector<std::vector<std::string>> failed_outputs(std::string_view ninjaOutput);
// Whether the output says a link failed, in either linker's words.
bool linker_failed(std::string_view ninjaOutput);

struct Tip {
    std::string domain;
    std::string text;
};
// The tips for a failure. `kept` names the BMIs the build directory keeps with
// the lines of an earlier source (`bmi-kept.list`).
std::vector<Tip> suspects(const Applied& applied, const std::vector<std::string>& kept, const Failed& failed);

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

namespace {

// A source as a step names it: relative to its package's root, or absolute.
// The plan's units are absolute, so one names the other when it is the tail.
bool names(std::string_view planned, std::string_view stated) {
    if (stated.empty()) return false;
    if (planned == stated) return true;
    return planned.size() > stated.size() && planned.ends_with(stated)
        && planned[planned.size() - stated.size() - 1] == '/';
}

std::string bmi_stem_of(std::string module) {
    std::ranges::replace(module, ':', '-');
    return module;
}

// The invocation that checks one entry, and the statement that keeps it.
std::string how_to_check(const registry::Entry& e, std::string_view check) {
    return std::format("{}={} mcpp build   ({} = \"{}\" keeps it)", e.env, check, e.key, check);
}

} // namespace

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

void write_applied(const std::filesystem::path& dir, const Applied& applied) {
    std::string text;
    if (!applied.fragments.empty()) text += std::format("check\t{}\n", applied.fragmentCheck);
    for (auto const& f : applied.fragments) {
        text += std::format("fragment\t{}", f.written);
        for (auto const& u : f.units) text += "\t" + u;
        text += "\n";
    }
    for (auto const& i : applied.importers) {
        text += std::format("imports\t{}", i.source);
        for (auto const& m : i.imports) text += "\t" + m;
        text += "\n";
    }
    const auto file = dir / kAppliedFile;
    std::string have;
    {
        std::ifstream in(file, std::ios::binary);
        have.assign(std::istreambuf_iterator<char>(in), {});
    }
    if (have == text) return;
    std::ofstream(file, std::ios::binary | std::ios::trunc) << text;
}

Applied read_applied(const std::filesystem::path& dir) {
    Applied a;
    std::ifstream in(dir / kAppliedFile, std::ios::binary);
    for (std::string line; std::getline(in, line);) {
        std::vector<std::string> cells;
        for (std::size_t at = 0;;) {
            const auto tab = line.find('\t', at);
            cells.push_back(line.substr(at, tab == std::string::npos ? tab : tab - at));
            if (tab == std::string::npos) break;
            at = tab + 1;
        }
        if (cells.size() < 2) continue;
        if (cells[0] == "check") a.fragmentCheck = cells[1];
        else if (cells[0] == "fragment") a.fragments.push_back({cells[1], {cells.begin() + 2, cells.end()}});
        else if (cells[0] == "imports") a.importers.push_back({cells[1], {cells.begin() + 2, cells.end()}});
    }
    return a;
}

std::vector<std::vector<std::string>> failed_outputs(std::string_view output) {
    std::vector<std::vector<std::string>> steps;
    while (!output.empty()) {
        const auto end = output.find('\n');
        auto line = output.substr(0, end);
        output = end == std::string_view::npos ? std::string_view{} : output.substr(end + 1);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
        if (!line.starts_with("FAILED:")) continue;
        line.remove_prefix(7);
        if (line.find("[code=") != std::string_view::npos) line = line.substr(line.find(']') + 1);
        std::vector<std::string> outputs;
        for (std::size_t at = 0; at < line.size();) {
            const auto next = line.find(' ', at);
            const auto word = line.substr(at, next == std::string_view::npos ? next : next - at);
            if (!word.empty()) outputs.emplace_back(word);
            if (next == std::string_view::npos) break;
            at = next + 1;
        }
        if (!outputs.empty()) steps.push_back(std::move(outputs));
    }
    return steps;
}

bool linker_failed(std::string_view output) {
    for (std::string_view words : {"undefined reference to", "undefined symbol", "multiple definition of",
                                    "duplicate symbol", "ld returned", "ld.lld: error", "lld-link: error"})
        if (output.find(words) != std::string_view::npos) return true;
    return false;
}

std::vector<Tip> suspects(const Applied& applied, const std::vector<std::string>& kept, const Failed& failed) {
    std::vector<Tip> tips;
    const auto* fragment = registry::find(registry::kPrivateFragment);
    const auto* cutoff = registry::find(registry::kGcmLocations);
    const std::string_view off = registry::kStandardEnv;

    // R1: the step that failed compiled a source built as units of its own.
    for (auto const& f : applied.fragments) {
        const bool hit = std::ranges::any_of(f.units, [&](const std::string& unit) {
            return std::ranges::any_of(failed.sources, [&](const std::string& s) { return names(unit, s); });
        });
        if (!hit || !fragment) continue;
        tips.push_back({"buildopt/private-fragment", std::format(
            "{} was built as two units (private-fragment), and one of them failed.\n"
            "     To check whether that is the cause: {}\n"
            "     If it builds, report the file; {}=off turns every optimization off for one run.",
            f.written, how_to_check(*fragment, applied.fragmentCheck.empty() ? fragment->check : applied.fragmentCheck),
            off)});
        break;
    }

    // R2: the step that failed compiled a unit that imports a BMI kept with the
    // lines of an earlier source.
    std::vector<std::string> keptStems;
    for (auto const& k : kept) keptStems.push_back(std::filesystem::path(k).stem().string());
    std::set<std::string> modules;
    for (auto const& i : applied.importers) {
        if (!std::ranges::any_of(failed.sources, [&](const std::string& s) { return names(i.source, s); })) continue;
        for (auto const& m : i.imports)
            if (std::ranges::contains(keptStems, bmi_stem_of(m))) modules.insert(m);
    }
    if (!modules.empty() && cutoff) {
        std::string list;
        std::size_t shown = 0;
        for (auto const& m : modules) {
            if (shown++ == 3) { list += ", ..."; break; }
            list += (list.empty() ? "" : ", ") + m;
        }
        tips.push_back({"buildopt/bmi-cutoff", std::format(
            "the BMI of module {} was kept although its source changed (bmi_cutoff), and a unit that imports it failed.\n"
            "     To check whether that is the cause: {}\n"
            "     If it builds, report the module; {}=off turns every optimization off for one run.",
            list, how_to_check(*cutoff, cutoff->check), off)});
    }

    // R3: a link failed, and the graph holds something either entry acted on.
    if (tips.empty() && failed.linked) {
        if (!applied.fragments.empty() && fragment)
            tips.push_back({"buildopt/private-fragment", std::format(
                "the link failed, and {} source(s) of this build are built as two units (private-fragment); the link cannot say whether that is the cause.\n"
                "     To check: {}", applied.fragments.size(),
                how_to_check(*fragment, applied.fragmentCheck.empty() ? fragment->check : applied.fragmentCheck))});
        if (!kept.empty() && cutoff)
            tips.push_back({"buildopt/bmi-cutoff", std::format(
                "the link failed, and {} BMI(s) of this build are kept with the lines of an earlier source (bmi_cutoff); the link cannot say whether that is the cause.\n"
                "     To check: {}", kept.size(), how_to_check(*cutoff, cutoff->check))});
    }
    return tips;
}

} // namespace mcpp::buildopt::advice
