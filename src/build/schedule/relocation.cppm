// mcpp.build.schedule.relocation — a compile writes its outputs at paths of
// its own attempt, and the executor publishes them by rename (design
// 2026-11-19 §10.6).
//
// WHY. A compiler that writes the published path directly lets one that
// outlives its build -- its controller killed, its process space gone --
// rewrite an output the next build has already trusted. Recovery then had to
// prove, by observing the compiler's unit, that no such writer remained, and
// where the unit could not be observed or was still occupied the next build
// was refused (mcpp#952). A compiler that writes only paths of its own attempt
// cannot reach a published output: the attempt is published by the executor
// after the compiler has ended, or discarded, and recovery needs no
// observation to protect what is published.
//
// HOW. The executor is told, on its command line, the files a compile writes
// (`--output`, `--depfile`, `--bmi`). Each word of the command that names one
// of them -- the whole word, or the part after `=` or `:` (`-fmodule-output=`,
// `/Fo:`), with nothing after it or a `.` suffix (`$out.d`, `$out.d.raw`) --
// is spelled below the attempt's directory instead, which mirrors the build
// directory. GCC names its BMI through its module map, so the attempt is given
// a copy of the map in which the unit's own module names the attempt's BMI.
// The auxiliary outputs a compiler names after the object (`.dwo`, `.gcno`) are
// named after the final object (`-dumpdir`, `-dumpbase`), because the object
// records their names and no prefix map rewrites them (measured, GCC 16.1 and
// clang 23.1.3): they are written in place, and no build input is among them.
//
// A file the command names that cannot be spelled below the attempt (an
// absolute path outside the build directory) leaves the whole compile in
// place: an attempt is either relocated in every output or in none, because
// recovery relies on the answer.
export module mcpp.build.schedule.relocation;

import std;
import mcpp.buildopt.keep;
import mcpp.home.files;

export namespace mcpp::build::schedule::relocation {

// The attempts' directory below a build directory: below it, and not beside
// `target/` with the compiler-run state, because a rename publishes and a
// rename does not cross file systems, which `target/<triple>` may.
inline constexpr std::string_view kAttempts = ".mcpp-attempts";

// What the executor is told of a compile's outputs, as its command spells
// them (relative to the build directory, where the executor runs).
struct Named {
    std::string output;    // `--output`
    std::string depfile;   // `--depfile`
    std::string bmi;       // `--bmi`
    bool object = false;   // `--object`: `output` is an object
};

struct Output {
    // A dependency file names what the compile read, the attempt's map among
    // them (GCC lists its mapper), and the attempt's outputs as its targets:
    // it is published with those names as published.
    enum class Kind { object, bmi, depfile, other };
    Kind kind = Kind::other;
    std::filesystem::path published, written;
};

// An attempt's relocation. Empty `outputs` is a compile in place.
struct Plan {
    std::filesystem::path directory;   // `.mcpp-attempts/<attempt>`
    std::filesystem::path root;        // `<directory>/out`, the mirror of the build directory
    std::vector<Output> outputs;
    // GCC: the unit's module map as the command named it, and the attempt's.
    std::string map, attemptMap;
    bool relocated() const { return !outputs.empty(); }
};

// The attempt's directory for an attempt name.
std::filesystem::path attempt_directory(std::string_view attempt);

// Rewrites `argv` (or one shell command line, whose words are separated by
// spaces) for the attempt `attempt`, with the auxiliary-name words of
// `auxNaming` ("dumpbase", "dumpdir", or "" for none) appended. On a command
// that cannot be relocated, the command is left as it was and the plan is
// empty; an error is a map that cannot be read or written.
std::expected<Plan, std::string> relocate(std::vector<std::string>& argv, const Named& named,
                                          std::string_view attempt, std::string_view auxNaming);
std::expected<Plan, std::string> relocate(std::string& command, const Named& named,
                                          std::string_view attempt, std::string_view auxNaming);

// The plan, written beside the attempt's outputs for a process that publishes
// them (the GCC schedule's supervisor), and read back.
std::expected<void, std::string> save(const Plan& plan);
std::expected<Plan, std::string> load(const std::filesystem::path& directory);

// Publishes what the compile wrote: an object or a BMI equal to the published
// one leaves the published file as it is (its time unchanged, so `restat`
// ends the rebuild there), and every other file the attempt wrote replaces
// its published counterpart. `withBmi` false leaves the BMI, which the GCC
// schedule publishes the moment it is complete. Does not discard.
std::expected<void, std::string> publish(const Plan& plan, bool withBmi = true);

// The BMI alone: whether it was there to publish.
std::expected<bool, std::string> publish_bmi(const Plan& plan);

// The attempt's BMI as written, or empty.
std::filesystem::path written_bmi(const Plan& plan);

// Removes the attempt's directory. A compiler that outlived its build may
// still hold a file of it, which a system refuses to remove while it is open:
// what remains is removed by a later build (`sweep`).
void discard(const std::filesystem::path& directory);

// Removes every attempt directory below `buildDir`: at a build's start, when
// no compile of the directory runs but one an interrupted build left.
void sweep(const std::filesystem::path& buildDir);

// The words that name the auxiliary outputs after `object`.
std::vector<std::string> aux_words(std::string_view auxNaming, std::string_view object);

}  // namespace mcpp::build::schedule::relocation

namespace mcpp::build::schedule::relocation {
namespace {

namespace fs = std::filesystem;

// Where `spelling` lies below the attempt: the same spelling below `root`, so
// that a word that names the file with a suffix (`$out.d.raw`) names the
// attempt's file with the same suffix. Empty for an absolute path outside the
// build directory.
std::string below(const fs::path& root, std::string_view spelling) {
    const fs::path p{std::string(spelling)};
    if (p.is_relative()) {
        auto rel = p.lexically_normal();
        if (rel.empty() || rel.begin()->string() == "..") return {};
        return (root / rel).generic_string();
    }
    std::error_code ec;
    const auto cwd = fs::current_path(ec);
    if (ec) return {};
    auto rel = p.lexically_normal().lexically_relative(cwd);
    if (rel.empty() || rel.begin()->string() == "..") return {};
    return (cwd / root / rel).string();
}

// Where in `word` a file `spelling` names begins: the whole word, or the part
// after an `=` or `:`, followed by nothing or by a `.` suffix.
std::optional<std::size_t> names_at(std::string_view word, std::string_view spelling) {
    if (spelling.empty()) return std::nullopt;
    auto at = [&](std::size_t start) {
        if (!word.substr(start).starts_with(spelling)) return false;
        const auto end = start + spelling.size();
        return end == word.size() || word[end] == '.';
    };
    if (at(0)) return 0;
    for (std::size_t i = 0; i < word.size(); ++i)
        if ((word[i] == '=' || word[i] == ':') && at(i + 1)) return i + 1;
    return std::nullopt;
}

struct Rewriter {
    const Named& named;
    Plan& plan;
    bool outside = false;   // a named file outside the build directory
    std::set<std::string> seen;

    Output::Kind kind_of(std::string_view file) const {
        if (!named.bmi.empty() && file == named.bmi) return Output::Kind::bmi;
        if (named.object && file == named.output) return Output::Kind::object;
        // `$out.d`, the filtered `$out.d.raw`, a scan's `$out.dep`.
        if (!named.depfile.empty() && file.starts_with(named.depfile)) return Output::Kind::depfile;
        return Output::Kind::other;
    }
    // The word as the attempt spells it; the word itself where it names none.
    std::string word(std::string_view w) {
        for (const auto* spelling : {&named.output, &named.depfile, &named.bmi}) {
            const auto start = names_at(w, *spelling);
            if (!start) continue;
            const auto file = w.substr(*start);
            const auto written = below(plan.root, file);
            if (written.empty()) { outside = true; return std::string(w); }
            if (seen.insert(std::string(file)).second)
                plan.outputs.push_back({kind_of(file), fs::path{std::string(file)}, fs::path{written}});
            return std::string(w.substr(0, *start)) + written;
        }
        return std::string(w);
    }
};

// GCC's module map, for the attempt: the line of the module whose BMI is
// `bmi` names the attempt's file, relative to the map's repository. Returns
// the attempt's map spelling, or empty where the map names no such module.
std::expected<std::string, std::string> attempt_map(std::string_view mapSpelling, const Named& named,
                                                    Plan& plan) {
    const fs::path map{std::string(mapSpelling)};
    std::ifstream in(map, std::ios::binary);
    if (!in) return std::unexpected(std::format("cannot read the module map {}", map.generic_string()));
    std::string out, line;
    fs::path repository;
    bool found = false;
    const auto bmi = fs::path{named.bmi}.lexically_normal();
    while (std::getline(in, line)) {
        const auto space = line.find(' ');
        if (space != std::string::npos && line.starts_with("$root ")) repository = line.substr(space + 1);
        else if (space != std::string::npos && !found) {
            const fs::path named_path = line.substr(space + 1);
            const auto resolved = (named_path.is_absolute() ? named_path : repository / named_path).lexically_normal();
            if (resolved == bmi) {
                const auto written = below(plan.root, named.bmi);
                if (written.empty()) return std::string{};
                line = line.substr(0, space + 1) + fs::path(written).lexically_relative(repository).generic_string();
                plan.outputs.push_back({Output::Kind::bmi, fs::path{named.bmi}, fs::path{written}});
                found = true;
            }
        }
        out += line + "\n";
    }
    if (!found) return std::string{};
    const auto attemptMap = plan.directory / "module.map";
    std::error_code ec;
    fs::create_directories(plan.directory, ec);
    std::ofstream file(attemptMap, std::ios::binary | std::ios::trunc);
    file << out;
    if (!file) return std::unexpected(std::format("cannot write the attempt's module map {}", attemptMap.generic_string()));
    return attemptMap.generic_string();
}

// The parent directories of what the attempt writes: a compiler does not
// create the directory of its `-o`.
std::expected<void, std::string> make_directories(const Plan& plan) {
    for (const auto& o : plan.outputs) {
        std::error_code ec;
        fs::create_directories(o.written.parent_path(), ec);
        if (ec) return std::unexpected(std::format("cannot create {}: {}", o.written.parent_path().generic_string(), ec.message()));
    }
    return {};
}

// Relocates `words` in place; `rejoin` writes them back where they came from.
std::expected<Plan, std::string> relocate_words(std::vector<std::string>& words, const Named& named,
                                                std::string_view attempt, std::string_view auxNaming) {
    Plan plan;
    plan.directory = attempt_directory(attempt);
    plan.root = plan.directory / "out";
    Rewriter rewrite{named, plan};
    std::vector<std::string> out;
    out.reserve(words.size() + 4);
    constexpr std::string_view mapper = "-fmodule-mapper=";
    bool bmiByMap = false;
    for (const auto& w : words) {
        if (!named.bmi.empty() && w.starts_with(mapper)) {
            auto map = attempt_map(std::string_view(w).substr(mapper.size()), named, plan);
            if (!map) return std::unexpected(map.error());
            if (map->empty()) { rewrite.outside = true; out.push_back(w); continue; }
            out.push_back(std::string(mapper) + *map);
            plan.map = w.substr(mapper.size());
            plan.attemptMap = *map;
            bmiByMap = true;
            rewrite.seen.insert(named.bmi);
            continue;
        }
        out.push_back(rewrite.word(w));
    }
    auto has = [&](std::string_view file) {
        return std::ranges::any_of(plan.outputs, [&](const Output& o) { return o.published == fs::path{std::string(file)}; });
    };
    // Every named file is written below the attempt, or the compile stays in
    // place: a compiler that wrote one of them where it is published would
    // void what recovery concludes from the relocation.
    const bool complete = !rewrite.outside && has(named.output)
        && (named.bmi.empty() || bmiByMap || has(named.bmi));
    if (!complete) {
        std::error_code ec;
        fs::remove_all(plan.directory, ec);
        return Plan{};
    }
    for (auto& w : aux_words(auxNaming, named.output)) out.push_back(std::move(w));
    if (auto made = make_directories(plan); !made) return std::unexpected(made.error());
    words = std::move(out);
    return plan;
}

std::string_view kind_name(Output::Kind k) {
    return k == Output::Kind::object ? "object" : k == Output::Kind::bmi ? "bmi"
         : k == Output::Kind::depfile ? "depfile" : "other";
}

// A dependency file with the attempt's names replaced by the published ones:
// the attempt's map by the unit's, and a path below the attempt's mirror by
// the same path below the build directory. Its other words are the compile's
// reads, as the compiler wrote them.
std::string published_text(const Plan& plan, std::string text) {
    auto replace_all = [&](const std::string& from, const std::string& to) {
        if (from.empty()) return;
        for (std::size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
            text.replace(at, from.size(), to);
    };
    replace_all(plan.attemptMap, plan.map);
    replace_all(plan.root.generic_string() + "/", "");
    return text;
}

// Moves `from` over `to`: one rename, so a reader sees the old file or the
// new one and never a part of either. Windows refuses the rename while another
// program holds the file, and a virus scanner opens a compile's output the
// moment it is written: a GCC BMI published from a cross build on a Windows
// runner was refused once among some 350 ("being used by another process").
// The refusal is retried for up to 30 s, longer than a scan of the largest
// output takes.
std::expected<void, std::string> replace(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::create_directories(to.parent_path(), ec);
    if (mcpp::home::replace_file_within(from, to, ec, std::chrono::seconds{30})) return {};
    return std::unexpected(std::format("cannot publish {} as {}: {}", from.generic_string(),
                                       to.generic_string(), ec.message()));
}

std::expected<void, std::string> publish_one(const Plan& plan, const Output& o) {
    std::error_code ec;
    if (!fs::is_regular_file(o.written, ec)) return {};
    if (o.kind == Output::Kind::depfile) {
        std::ifstream in(o.written, std::ios::binary);
        const std::string text{std::istreambuf_iterator<char>(in), {}};
        in.close();
        std::ofstream out(o.written, std::ios::binary | std::ios::trunc);
        out << published_text(plan, text);
        if (!out) return std::unexpected(std::format("cannot write {}", o.written.generic_string()));
    }
    bool keep = false;
    if (fs::is_regular_file(o.published, ec)) {
        if (o.kind == Output::Kind::object)
            keep = mcpp::buildopt::keep::keeps_previous_object(o.written, o.published, o.published);
        else if (o.kind == Output::Kind::bmi)
            keep = mcpp::buildopt::keep::keeps_previous(o.written, o.published, o.published);
    }
    if (keep) {
        fs::remove(o.written, ec);
        return {};
    }
    return replace(o.written, o.published);
}

}  // namespace

fs::path attempt_directory(std::string_view attempt) {
    return fs::path{std::string(kAttempts)} / std::string(attempt);
}

std::vector<std::string> aux_words(std::string_view auxNaming, std::string_view object) {
    if (auxNaming.empty() || object.empty()) return {};
    const auto slash = object.find_last_of("/\\");
    const std::string dir = slash == std::string_view::npos ? "." : std::string(object.substr(0, slash));
    std::string stem(slash == std::string_view::npos ? object : object.substr(slash + 1));
    if (const auto dot = stem.rfind('.'); dot != std::string::npos && dot > 0) stem.resize(dot);
    if (auxNaming == "dumpbase") return {"-dumpdir", dir + "/", "-dumpbase", stem};
    if (auxNaming == "dumpdir") return {"-dumpdir", dir + "/" + stem + "."};
    return {};
}

std::expected<Plan, std::string> relocate(std::vector<std::string>& argv, const Named& named,
                                          std::string_view attempt, std::string_view auxNaming) {
    return relocate_words(argv, named, attempt, auxNaming);
}

std::expected<Plan, std::string> relocate(std::string& command, const Named& named,
                                          std::string_view attempt, std::string_view auxNaming) {
    // The command's words as the backend joined them: separated by single
    // spaces, a quoted word kept whole. A word that names an output is a path
    // of the build directory, spelled without a character a shell quotes.
    std::vector<std::string> words;
    std::string current;
    char quote = 0;
    for (const char c : command) {
        if (quote == 0 && (c == '"' || c == '\'')) quote = c;
        else if (c == quote) quote = 0;
        if (c == ' ' && quote == 0) { words.push_back(std::move(current)); current.clear(); continue; }
        current += c;
    }
    words.push_back(std::move(current));
    auto plan = relocate_words(words, named, attempt, auxNaming);
    if (!plan || !plan->relocated()) return plan;
    std::string joined;
    for (const auto& w : words) joined += (joined.empty() ? "" : " ") + w;
    command = std::move(joined);
    return plan;
}

std::expected<void, std::string> save(const Plan& plan) {
    std::string text = std::format("map\t{}\t{}\n", plan.map, plan.attemptMap);
    for (const auto& o : plan.outputs)
        text += std::format("{}\t{}\t{}\n", kind_name(o.kind), o.published.generic_string(), o.written.generic_string());
    std::error_code ec;
    fs::create_directories(plan.directory, ec);
    const auto file = plan.directory / "plan";
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
    if (!out) return std::unexpected(std::format("cannot write the attempt's plan {}", file.generic_string()));
    return {};
}

std::expected<Plan, std::string> load(const fs::path& directory) {
    Plan plan;
    plan.directory = directory;
    plan.root = directory / "out";
    std::ifstream in(directory / "plan", std::ios::binary);
    if (!in) return std::unexpected(std::format("cannot read the attempt's plan in {}", directory.generic_string()));
    for (std::string line; std::getline(in, line);) {
        const auto a = line.find('\t'), b = line.rfind('\t');
        if (a == std::string::npos || a == b) return std::unexpected("invalid attempt plan");
        const auto kind = line.substr(0, a);
        if (kind == "map") {
            plan.map = line.substr(a + 1, b - a - 1);
            plan.attemptMap = line.substr(b + 1);
            continue;
        }
        plan.outputs.push_back({kind == "object" ? Output::Kind::object : kind == "bmi" ? Output::Kind::bmi
                                : kind == "depfile" ? Output::Kind::depfile : Output::Kind::other,
                                fs::path{line.substr(a + 1, b - a - 1)}, fs::path{line.substr(b + 1)}});
    }
    return plan;
}

std::expected<void, std::string> publish(const Plan& plan, bool withBmi) {
    for (const auto& o : plan.outputs) {
        if (o.kind == Output::Kind::bmi && !withBmi) continue;
        if (auto done = publish_one(plan, o); !done) return done;
    }
    // What else the compiler wrote below the attempt, named after a file it
    // was given (clang's `-ftime-trace` beside the object): its counterpart.
    std::error_code ec;
    if (!fs::is_directory(plan.root, ec)) return {};
    std::set<fs::path> named;
    for (const auto& o : plan.outputs) named.insert(o.written.lexically_normal());
    for (fs::recursive_directory_iterator it(plan.root, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code fec;
        if (!it->is_regular_file(fec) || named.contains(it->path().lexically_normal())) continue;
        const auto rel = it->path().lexically_relative(plan.root);
        if (auto done = replace(it->path(), rel); !done) return done;
    }
    return {};
}

std::expected<bool, std::string> publish_bmi(const Plan& plan) {
    for (const auto& o : plan.outputs) {
        if (o.kind != Output::Kind::bmi) continue;
        std::error_code ec;
        if (!fs::is_regular_file(o.written, ec)) return false;
        if (auto done = publish_one(plan, o); !done) return std::unexpected(done.error());
        return true;
    }
    return false;
}

fs::path written_bmi(const Plan& plan) {
    for (const auto& o : plan.outputs)
        if (o.kind == Output::Kind::bmi) return o.written;
    return {};
}

void discard(const fs::path& directory) {
    std::error_code ec;
    fs::remove_all(directory, ec);
}

void sweep(const fs::path& buildDir) {
    std::error_code ec;
    const auto attempts = buildDir / std::string(kAttempts);
    for (fs::directory_iterator it(attempts, ec), end; !ec && it != end; it.increment(ec)) discard(it->path());
    fs::remove(attempts, ec);
}

}  // namespace mcpp::build::schedule::relocation
