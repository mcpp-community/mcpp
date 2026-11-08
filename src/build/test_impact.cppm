// mcpp.build.test_impact -- the tests a change affects (docs/08 "The tests a
// change affects"), read from the plan: a test is affected by a change of a
// file it is built from -- its own files, the module units it imports and
// the units they import in turn, and every unit of its closure that is not a
// module unit -- and by a change of what decides how everything is built
// (a manifest, a lock, a build program, a header). Another file affects the
// tests whose own files name it. Selection by change narrows; it never adds
// a test that a pattern or `-p` left out.
export module mcpp.build.test_impact;

import std;
import mcpp.build.plan;
import mcpp.modgraph.graph;
import mcpplibs.os;

export namespace mcpp::build::test_impact {

struct Affected {
    // The affected tests, each with the first reason found.
    std::map<std::string, std::string> tests;
    // Set when every test is affected, with the reason.
    std::optional<std::string> everything;
    // What a changed path decided, for the run's report.
    std::vector<std::string> notes;
};

// `changed`: paths relative to `plan.projectRoot`, in forward slashes.
Affected affected_tests(const BuildPlan& plan, const std::vector<std::string>& changed);

// The paths below `root` that differ from `base`: committed since it, changed
// in the working tree, and new and not ignored (`git diff --name-only
// --relative <base>` and `git ls-files --others --exclude-standard`).
std::expected<std::vector<std::string>, std::string>
changed_since(const std::filesystem::path& root, std::string_view base);

}  // namespace mcpp::build::test_impact

namespace mcpp::build::test_impact {

namespace {

std::string primary(std::string_view name) {
    return std::string(name.substr(0, name.find(':')));
}

bool decides_the_build(const std::filesystem::path& p) {
    const auto name = p.filename().string();
    return name == "mcpp.toml" || name == "mcpp.lock" || p.extension() == ".mcpp";
}

bool is_header(const std::filesystem::path& p) {
    static const std::set<std::string> kinds{".h", ".hh", ".hpp", ".hxx", ".h++", ".inc",
                                             ".inl", ".ipp", ".tcc", ".tpp", ".def"};
    return kinds.contains(p.extension().string());
}

bool is_source(const std::filesystem::path& p) {
    static const std::set<std::string> kinds{".cpp", ".cc", ".cxx", ".c++", ".c", ".cppm",
                                             ".ixx", ".mpp", ".mxx", ".cu", ".m", ".mm",
                                             ".s", ".S", ".asm"};
    return kinds.contains(p.extension().string());
}

std::string read_text(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

// The module the unit belongs to: the one it provides (an interface or a
// partition), or, of an implementation unit (`module M;`), M.
std::string module_of(const CompileUnit& cu) {
    if (!cu.providesModule.empty()) return primary(cu.providesModule);
    if (cu.declaration == mcpp::modgraph::ModuleDeclaration::Implementation && !cu.imports.empty())
        return primary(cu.imports.front());
    return {};
}

// The files a test is built from.
std::set<std::filesystem::path> closure_of(const BuildPlan& plan, const LinkUnit& lu,
                                           const std::map<std::string, std::vector<const CompileUnit*>>& units,
                                           const std::map<std::filesystem::path, const CompileUnit*>& byObject) {
    std::set<std::filesystem::path> files;
    std::vector<const CompileUnit*> work;
    std::set<std::string> modules;
    auto visit = [&](const CompileUnit* cu) {
        if (files.insert(cu->source.lexically_normal()).second) work.push_back(cu);
    };
    for (auto const& object : lu.objects) {
        auto it = byObject.find(object);
        if (it == byObject.end()) continue;
        // A unit of no module is linked whether or not anything imports it.
        if (module_of(*it->second).empty()) visit(it->second);
    }
    for (auto const& own : lu.ownSources)
        if (auto it = std::ranges::find(plan.compileUnits, own, &CompileUnit::source); it != plan.compileUnits.end())
            visit(&*it);
    while (!work.empty()) {
        const auto* cu = work.back();
        work.pop_back();
        for (auto const& name : cu->imports) {
            if (!modules.insert(primary(name)).second) continue;
            if (auto it = units.find(primary(name)); it != units.end())
                std::ranges::for_each(it->second, visit);
        }
    }
    return files;
}

// The test's own file that names `rel` (or its file name), if one does.
std::optional<std::filesystem::path> naming_file(const LinkUnit& lu, const std::string& rel, const std::string& name) {
    for (auto const& own : lu.ownSources) {
        const auto text = read_text(own);
        if (text.find(rel) != std::string::npos || text.find(name) != std::string::npos) return own;
    }
    return std::nullopt;
}

}  // namespace

Affected affected_tests(const BuildPlan& plan, const std::vector<std::string>& changed) {
    Affected out;
    std::map<std::string, std::vector<const CompileUnit*>> units;
    std::map<std::filesystem::path, const CompileUnit*> byObject, bySource;
    for (auto const& cu : plan.compileUnits) {
        if (auto m = module_of(cu); !m.empty()) units[m].push_back(&cu);
        byObject.emplace(cu.object, &cu);
        bySource.emplace(cu.source.lexically_normal(), &cu);
    }
    std::vector<std::pair<const LinkUnit*, std::set<std::filesystem::path>>> tests;
    for (auto const& lu : plan.linkUnits)
        if (lu.kind == LinkUnit::TestBinary) tests.emplace_back(&lu, closure_of(plan, lu, units, byObject));

    auto affect = [&](const std::string& test, std::string why) { out.tests.emplace(test, std::move(why)); };
    for (auto const& rel : changed) {
        const auto path = (plan.projectRoot / mcpplibs::os::fs::from_utf8(rel)).lexically_normal();
        if (decides_the_build(path) || is_header(path)
            || (is_source(path) && !bySource.contains(path))) {
            out.everything = std::format("{} {}", rel,
                decides_the_build(path) ? "decides how the tests are built"
                : is_header(path)       ? "is a header, and which units include it is not known"
                                        : "is a source this build does not plan");
            out.notes.push_back(*out.everything);
            break;
        }
        std::size_t hits = 0;
        for (auto const& [lu, files] : tests)
            if (files.contains(path)) {
                affect(lu->targetName, std::format("it is built from {}", rel));
                ++hits;
            }
        if (bySource.contains(path)) {
            out.notes.push_back(std::format("{}: {} test{}", rel, hits, hits == 1 ? "" : "s"));
            continue;
        }
        // Another file: the tests whose own files name it.
        const auto name = path.filename().generic_string();
        for (auto const& [lu, _] : tests)
            if (auto own = naming_file(*lu, rel, name)) {
                affect(lu->targetName, std::format("{} names {}", own->filename().generic_string(), rel));
                ++hits;
            }
        out.notes.push_back(std::format("{}: {} test{} name{} it", rel, hits, hits == 1 ? "" : "s",
                                        hits == 1 ? "s" : ""));
    }
    if (out.everything)
        for (auto const& [lu, _] : tests) out.tests.emplace(lu->targetName, *out.everything);
    return out;
}

std::expected<std::vector<std::string>, std::string>
changed_since(const std::filesystem::path& root, std::string_view base) {
    std::set<std::string> paths;
    auto git = [&](std::vector<std::string> args) -> std::expected<void, std::string> {
        const auto command = args.front();
        mcpplibs::os::process::spawn_options o;
        o.program = "git";
        o.args = std::move(args);
        o.cwd = root;
        o.in = mcpplibs::os::process::stdio::null;
        auto r = mcpplibs::os::process::run(std::move(o));
        if (!r || !r->status.success())
            return std::unexpected(std::format("git {} did not answer{}", command,
                                               r ? ": " + r->err : std::string{}));
        std::istringstream lines(r->out);
        for (std::string line; std::getline(lines, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) paths.insert(line);
        }
        return {};
    };
    if (auto r = git({"diff", "--name-only", "--relative", std::string(base)}); !r) return std::unexpected(r.error());
    if (auto r = git({"ls-files", "--others", "--exclude-standard"}); !r) return std::unexpected(r.error());
    return std::vector<std::string>(paths.begin(), paths.end());
}

}  // namespace mcpp::build::test_impact
