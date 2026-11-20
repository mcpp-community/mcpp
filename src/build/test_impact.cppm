// mcpp.build.test_impact -- the tests a change affects (docs/08 "The tests a
// change affects"), read from the plan: a test is affected by a change of a
// file it is built from -- its own files, the module units it imports and
// the units they import in turn, and every unit of its closure that is not a
// module unit -- and by a change of what decides how everything is built
// (a manifest, a lock, a build program, a header). A test that runs a program
// of the package (a file it is built from names `MCPP_BIN_`) is affected by a
// change of any source of the package that is not a test's own. A header affects
// the tests built from a unit that read it when the build directory last built it
// (ninja's deps log and the scans' depfiles, design 2026-11-20 §5.11), and every
// test when the directory holds no such record. Another file affects the tests
// whose own files name it. Selection by change narrows; it never adds a test that
// a pattern or `-p` left out.
export module mcpp.build.test_impact;

import std;
import mcpp.build.plan;
import mcpp.modgraph.graph;
import mcpp.build.unit_cache;   // depfile_prerequisites
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
        // A unit built for a source file is built from that file.
        if (!cu->writtenAs.empty()) files.insert(cu->writtenAs.lexically_normal());
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

// THE FILES EACH OUTPUT READ, as ninja's deps log states them (`.ninja_deps`, format 4: a magic
// line, a version, then records whose size word's high bit tells a dependency record from a
// path record). A later record of an output replaces an earlier one, as in ninja. Nothing when
// the log is absent or of another version.
std::map<std::string, std::vector<std::string>> ninja_deps(const std::filesystem::path& file) {
    std::map<std::string, std::vector<std::string>> out;
    const auto bytes = read_text(file);
    constexpr std::string_view kMagic = "# ninjadeps\n";
    if (!bytes.starts_with(kMagic) || bytes.size() < kMagic.size() + 4) return out;
    auto word = [&](std::size_t at) {
        std::uint32_t v = 0;
        std::memcpy(&v, bytes.data() + at, 4);
        return v;
    };
    if (word(kMagic.size()) != 4) return out;
    std::vector<std::string> paths;
    for (std::size_t at = kMagic.size() + 4; at + 4 <= bytes.size();) {
        const auto head = word(at);
        const std::size_t size = head & 0x7FFFFFFFu;
        at += 4;
        if (at + size > bytes.size() || size < 4) break;
        if ((head & 0x80000000u) == 0) {
            std::string path = bytes.substr(at, size - 4);
            while (!path.empty() && path.back() == '\0') path.pop_back();
            paths.push_back(std::move(path));
        } else if (const auto id = word(at); id < paths.size() && size >= 12) {
            auto& inputs = out[paths[id]];
            inputs.clear();
            for (std::size_t i = at + 12; i + 4 <= at + size; i += 4)
                if (const auto in = word(i); in < paths.size()) inputs.push_back(paths[in]);
        }
        at += size;
    }
    return out;
}

// The units that read each file, from the build directory's records: an object's inputs in the
// deps log, a scan's depfile (`<object dir>/<source name>.ddi.dep`). `complete` when every unit
// of a file under the project's root has a record: only then does a header no record names
// affect no test.
struct Readers {
    std::map<std::filesystem::path, std::vector<const CompileUnit*>> of;
    bool complete = true;
};
Readers readers_of(const BuildPlan& plan) {
    Readers out;
    const auto deps = ninja_deps(plan.outputDir / ".ninja_deps");
    auto add = [&](const std::filesystem::path& file, const CompileUnit* cu) {
        const auto abs = (file.is_absolute() ? file : plan.outputDir / file).lexically_normal();
        auto& units = out.of[abs];
        if (units.empty() || units.back() != cu) units.push_back(cu);
    };
    const auto root = plan.projectRoot.lexically_normal().generic_string() + "/";
    for (auto const& cu : plan.compileUnits) {
        bool recorded = false;
        if (auto it = deps.find(cu.object.generic_string()); it != deps.end()) {
            recorded = true;
            for (auto const& in : it->second) add(mcpplibs::os::fs::from_utf8(in), &cu);
        }
        const auto scan = plan.outputDir / cu.object.parent_path() / (cu.source.filename().string() + ".ddi.dep");
        if (const auto text = read_text(scan); !text.empty()) {
            recorded = true;
            for (auto const& in : mcpp::build::unit_cache::depfile_prerequisites(text))
                add(mcpplibs::os::fs::from_utf8(in), &cu);
        }
        if (!recorded && cu.source.lexically_normal().generic_string().starts_with(root)) out.complete = false;
    }
    return out;
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
        if (!cu.writtenAs.empty()) bySource.emplace(cu.writtenAs.lexically_normal(), &cu);
    }
    std::vector<std::pair<const LinkUnit*, std::set<std::filesystem::path>>> tests;
    for (auto const& lu : plan.linkUnits)
        if (lu.kind == LinkUnit::TestBinary) tests.emplace_back(&lu, closure_of(plan, lu, units, byObject));

    auto affect = [&](const std::string& test, std::string why) { out.tests.emplace(test, std::move(why)); };
    // THE TESTS THAT RUN THE PACKAGE'S PROGRAMS. An end-to-end test is built
    // from its own files and the support it imports, and runs the program it
    // tests from `MCPP_BIN_<NAME>`: nothing of the program is in its closure,
    // and a change of the program selected none of them (measured on the e2e
    // programs of mcpp: a change of src/ selected the unit tests alone).
    std::map<std::filesystem::path, bool> namesProgram;
    auto names_program = [&](const std::filesystem::path& f) {
        auto [it, fresh] = namesProgram.try_emplace(f, false);
        if (fresh) it->second = read_text(f).find("MCPP_BIN_") != std::string::npos;
        return it->second;
    };
    // Read once, and only when a header changed.
    std::optional<Readers> readers;
    std::vector<const LinkUnit*> runners;
    std::set<std::filesystem::path> testsOwn;
    for (auto const& [lu, files] : tests) {
        for (auto const& own : lu->ownSources) testsOwn.insert(own.lexically_normal());
        if (std::ranges::any_of(files, names_program)) runners.push_back(lu);
    }
    for (auto const& rel : changed) {
        const auto path = (plan.projectRoot / mcpplibs::os::fs::from_utf8(rel)).lexically_normal();
        if (is_header(path) && !readers) readers = readers_of(plan);
        const bool unknownHeader = is_header(path) && !readers->complete;
        if (decides_the_build(path) || unknownHeader
            || (is_source(path) && !bySource.contains(path))) {
            out.everything = std::format("{} {}", rel,
                decides_the_build(path) ? "decides how the tests are built"
                : unknownHeader         ? "is a header, and the build directory has no record of what some unit read"
                                        : "is a source this build does not plan");
            out.notes.push_back(*out.everything);
            break;
        }
        std::size_t hits = 0;
        if (is_header(path)) {
            const auto it = readers->of.find(path);
            const std::size_t units = it == readers->of.end() ? 0 : it->second.size();
            for (auto const& [lu, files] : tests) {
                const bool reads = it != readers->of.end() && std::ranges::any_of(it->second, [&](const CompileUnit* cu) {
                    return files.contains(cu->source.lexically_normal());
                });
                if (reads) {
                    affect(lu->targetName, std::format("it is built from a unit that read {}", rel));
                    ++hits;
                }
            }
            // A unit of the package that is no test's own: the tests that run its programs.
            const bool programReads = it != readers->of.end() && std::ranges::any_of(it->second, [&](const CompileUnit* cu) {
                return !testsOwn.contains(cu->source.lexically_normal());
            });
            for (const auto* lu : runners)
                if (programReads && !out.tests.contains(lu->targetName)) {
                    affect(lu->targetName, std::format("it runs a program of the package, which read {}", rel));
                    ++hits;
                }
            out.notes.push_back(std::format("{}: read by {} unit{}, {} test{}", rel, units, units == 1 ? "" : "s",
                                            hits, hits == 1 ? "" : "s"));
            continue;
        }
        for (auto const& [lu, files] : tests)
            if (files.contains(path)) {
                affect(lu->targetName, std::format("it is built from {}", rel));
                ++hits;
            }
        if (bySource.contains(path) && !testsOwn.contains(path))
            for (const auto* lu : runners)
                if (!out.tests.contains(lu->targetName)) {
                    affect(lu->targetName, std::format("it runs a program of the package, and {} is a source of it", rel));
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
