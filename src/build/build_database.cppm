// mcpp.build.build_database — the plan, rendered as an S1 build database.
//
// S1 ("C++ Build Database: IDE Profile", profile 0.2.0) is a profile of the
// WG21 P2977R2 build database format; docs/specs/build-database.md (SPEC-005)
// states what mcpp guarantees as its producer. The document is a projection of
// what prepare_build planned: every field is read from a BuildContext, and
// nothing in this module compiles, runs or writes anything.
//
// Three rules shape the projection, and each one is a statement about the
// engine rather than about the format:
//
//   - `arguments` come from the record the compile database renders
//     (mcpp.build.compile_commands::unit_invocations), so the two databases
//     cannot list different command lines for one unit.
//   - `visible-sets` is every other set of the planned member. The engine
//     resolves imports over one graph per invocation; a narrower closure would
//     describe a rule the build does not enforce.
//   - The standard library modules are translation units of their own set,
//     carrying the command mcpp runs to build them. That is one rule for every
//     toolchain and for a package that ships its own `std.cppm`, and S1 lets
//     units outrank a module manifest.

export module mcpp.build.build_database;

import std;
import mcpp.build.build_program;
import mcpp.build.compile_commands;
import mcpp.build.flags;
import mcpp.build.plan;
import mcpp.build.prepare;
import mcpp.home;
import mcpp.libs.json;
import mcpp.manifest;
import mcpp.modgraph.graph;
import mcpp.modgraph.scanner;
import mcpp.platform;
import mcpp.toolchain.fingerprint;
import mcpp.toolchain.linkmodel;
import mcpp.toolchain.model;
import mcpp.toolchain.stdmod;

export namespace mcpp::build::database {

inline constexpr std::string_view kProfileVersion = "0.2.0";
inline constexpr std::string_view kStdSetName      = "mcpp:std";

// One planned member of the document.
struct Member {
    const BuildContext*   ctx = nullptr;
    // Prepended to every set name: empty for a single package, `<member>/` for
    // a member of a workspace document.
    std::string           setPrefix;
    // Where the planning pass wrote (BuildOverrides::work_dir): the build
    // programs' caches, and so their declared inputs, are under it.
    std::filesystem::path workDir;
    // Test discovery (`[test] discover`, relative to `testRoot`): a new file
    // matching it is a new test target, and so a new unit in the document.
    std::filesystem::path    testRoot;
    std::vector<std::string> testDiscover;
};

struct Rendered {
    nlohmann::json            database;
    // The same units as a JSON Compilation Database: the entries `mcpp build
    // --configure-only` writes to compile_commands.json, from the same record.
    nlohmann::json            compileCommands = nlohmann::json::array();
    std::vector<std::string>  watch;
    std::string               inputsFingerprint;
    // Conditions found while rendering; reported as warnings by the command.
    std::vector<PlanNote>     notes;
};

// The S1 document for `members`, and the inputs whose change changes it.
// `workspaceRoot` anchors the relative `watch` patterns; `selector` is the
// command's own selection (target, toolchain, profile, members), which enters
// the fingerprint because the same files answer differently under another one.
Rendered render(std::span<const Member> members,
                const std::filesystem::path& workspaceRoot,
                std::string_view selector);

// ── Exported for tests: each is a rule SPEC-005 states. ──────────────────

// The S1 role of a declaration form (S1 §8.2).
std::string_view role_name(mcpp::modgraph::ModuleDeclaration declaration);

// The S1 `stdlib.name` for mcpp's standard library identifier.
std::string_view stdlib_name(std::string_view stdlibId);

// `<family>-<version>-<target triple>`, with mcpp's family name (`llvm`, as in
// `llvm@22.1.8`) and the triple as the compiler spells it: stable within a
// document, and opaque to a consumer (S1 section 5.1).
std::string toolchain_id(const mcpp::toolchain::Toolchain& tc,
                         std::string_view compilerTriple);

// Splits a command string mcpp rendered for the host shell into words, undoing
// its quoting: POSIX `sh` rules, or the Microsoft C runtime's rules on Windows.
// The reader is mcpp::manifest::host_command_words; this name is kept for the
// standard library units' recovery below.
std::vector<std::string> split_command_words(std::string_view command, bool windows);

// The working directory and argument vector of the command in `commands` whose
// words name `source`, recovered from the rendering: a leading `cd`, an `env`
// word and environment assignments, and redirections are removed. A driver
// path that the rendering left unquoted despite a space is rejoined. Empty when
// no command names the source.
struct Invocation {
    std::filesystem::path    workDirectory;
    std::vector<std::string> arguments;
};
std::optional<Invocation> recover_invocation(const std::vector<std::string>& commands,
                                             const std::filesystem::path& source,
                                             const std::filesystem::path& driver,
                                             const std::filesystem::path& defaultDirectory,
                                             bool windows);

} // namespace mcpp::build::database

namespace mcpp::build::database {

namespace {

std::string native_string(const std::filesystem::path& p) {
    auto n = p;
    n.make_preferred();
    return n.string();
}

std::string qualified_name(const mcpp::manifest::Manifest& m) {
    return m.package.namespace_.empty()
        ? m.package.name
        : m.package.namespace_ + "." + m.package.name;
}

bool is_assignment(std::string_view w) {
    auto eq = w.find('=');
    if (eq == std::string_view::npos || eq == 0) return false;
    if (!(std::isalpha(static_cast<unsigned char>(w[0])) || w[0] == '_')) return false;
    for (std::size_t i = 1; i < eq; ++i) {
        const unsigned char c = static_cast<unsigned char>(w[i]);
        if (!(std::isalnum(c) || c == '_')) return false;
    }
    return true;
}

// `2>&1`, `>file`, `<NUL`, `2>nul`, `</dev/null`: one word. `>`, `2>`, `<`,
// `>>`: the target is the next word.
enum class Redirect { None, Attached, Detached };
Redirect redirect_kind(std::string_view w) {
    std::size_t i = 0;
    while (i < w.size() && std::isdigit(static_cast<unsigned char>(w[i]))) ++i;
    if (i >= w.size() || (w[i] != '>' && w[i] != '<')) return Redirect::None;
    std::size_t j = i + 1;
    if (j < w.size() && w[j] == w[i]) ++j;          // `>>`
    return j == w.size() ? Redirect::Detached : Redirect::Attached;
}

bool names_path(std::string_view word, const std::filesystem::path& path,
                  const std::filesystem::path& cwd) {
    std::filesystem::path w{std::string(word)};
    const auto want = path.lexically_normal();
    if (w.lexically_normal() == want) return true;
    if (!w.is_absolute() && !cwd.empty() && (cwd / w).lexically_normal() == want)
        return true;
    return false;
}

// S1 `config-files`: the configuration files the driver reads without being
// named on a command line. A clang driver reads the `.cfg` beside it unless a
// unit passes `--no-default-config`, which mcpp does whenever that file exists
// (mcpp.toolchain.linkmodel::resolve_clang_driver). A GCC driver reads the
// `specs` file in its library directory for its own machine and version, which
// a distribution may spell with the major version only. Found from the layout
// the driver searches; no driver is run.
nlohmann::json config_files(const mcpp::toolchain::Toolchain& tc,
                            const std::vector<mcpp::build::UnitInvocation>& invocations) {
    nlohmann::json out = nlohmann::json::array();
    using C = mcpp::toolchain::CompilerId;
    std::error_code ec;
    if (tc.compiler == C::Clang) {
        const auto dm = mcpp::toolchain::resolve_clang_driver(tc);
        const bool read = std::ranges::any_of(invocations, [](auto const& inv) {
            return std::ranges::find(inv.arguments, "--no-default-config") == inv.arguments.end();
        });
        if (dm.hasCfg && read) out.push_back(native_string(dm.cfgPath));
    } else if (tc.compiler == C::GCC && !tc.targetTriple.empty()) {
        const auto machine = tc.binaryPath.parent_path().parent_path()
                           / "lib" / "gcc" / tc.targetTriple;
        const auto major = tc.version.substr(0, tc.version.find('.'));
        for (auto const& version : {tc.version, major}) {
            const auto specs = machine / version / "specs";
            if (!version.empty() && std::filesystem::is_regular_file(specs, ec)) {
                out.push_back(native_string(specs));
                break;
            }
        }
    }
    return out;
}

// S1 `baseline-arguments` and `local-arguments`. A unit's arguments are its
// driver, then the set's baseline, then its local arguments, then its own
// trailing `-c <source> -o <object>` when it has one: the baseline is the longest
// prefix that every unit of the set shares after the driver and without that
// tail. A prefix keeps the order of the arguments, which decides include search
// and macro definitions; a common subset would not.
void split_baseline(nlohmann::json& set) {
    std::vector<std::vector<std::string>> semantic;
    for (auto const& u : set["translation-units"]) {
        auto args = u["arguments"].get<std::vector<std::string>>();
        std::vector<std::string> rest(args.size() > 1 ? args.begin() + 1 : args.end(), args.end());
        const std::filesystem::path cwd{u["work-directory"].get<std::string>()};
        const auto n = rest.size();
        if (n >= 4 && rest[n - 4] == "-c" && rest[n - 2] == "-o"
            && names_path(rest[n - 3], u["source"].get<std::string>(), cwd)
            && names_path(rest[n - 1], u["object"].get<std::string>(), cwd))
            rest.resize(n - 4);
        semantic.push_back(std::move(rest));
    }
    std::vector<std::string> baseline;
    if (!semantic.empty()) {
        baseline = semantic.front();
        for (auto const& unit : semantic) {
            std::size_t k = 0;
            while (k < baseline.size() && k < unit.size() && baseline[k] == unit[k]) ++k;
            baseline.resize(k);
        }
    }
    std::size_t i = 0;
    for (auto& u : set["translation-units"]) {
        const auto& unit = semantic[i++];
        u["local-arguments"] = std::vector<std::string>(
            unit.begin() + static_cast<std::ptrdiff_t>(baseline.size()), unit.end());
    }
    set["baseline-arguments"] = std::move(baseline);
}

nlohmann::json toolchain_json(const mcpp::toolchain::Toolchain& tc,
                              std::string_view compilerTriple,
                              const std::vector<mcpp::build::UnitInvocation>& invocations) {
    nlohmann::json j{
        {"family",       std::string(tc.compiler_name())},
        {"version",      tc.version},
        {"driver",       tc.binaryPath.string()},
        {"target",       std::string(compilerTriple)},
        {"config-files", config_files(tc, invocations)},
    };
    if (!tc.sysroot.empty()) j["sysroot"] = native_string(tc.sysroot);
    if (!tc.stdlibId.empty()) {
        nlohmann::json stdlib{{"name", std::string(stdlib_name(tc.stdlibId))}};
        if (!tc.stdlibVersion.empty()) stdlib["version"] = tc.stdlibVersion;
        j["stdlib"] = std::move(stdlib);
    }
    return j;
}

std::string target_kind(const mcpp::manifest::Manifest& m) {
    bool library = false, program = false;
    for (auto const& t : m.targets) {
        using K = mcpp::manifest::Target::Kind;
        if (t.kind == K::Library || t.kind == K::SharedLibrary) library = true;
        if (t.kind == K::Binary || t.kind == K::Application)    program = true;
    }
    return library ? "library" : program ? "executable" : "other";
}

// The `watch` spelling of a path: relative to the workspace root, with `/`, when
// it is under it; absolute and native otherwise (S2 accepts no absolute glob).
std::optional<std::string> relative_to(const std::filesystem::path& p,
                                       const std::filesystem::path& root) {
    auto rel = p.lexically_normal().lexically_relative(root.lexically_normal());
    if (rel.empty()) return std::nullopt;
    auto first = rel.begin();
    if (first != rel.end() && *first == "..") return std::nullopt;
    auto s = rel.generic_string();
    return s == "." ? std::string{} : s;
}

struct SetData {
    std::string    familyName;
    std::string    kind;
    nlohmann::json units = nlohmann::json::array();
};

} // namespace

std::string_view role_name(mcpp::modgraph::ModuleDeclaration declaration) {
    using D = mcpp::modgraph::ModuleDeclaration;
    switch (declaration) {
        case D::None:                    return "non-module";
        case D::Interface:               return "module-interface";
        case D::InterfacePartition:      return "module-partition-interface";
        case D::ImplementationPartition: return "module-partition-implementation";
        case D::Implementation:          return "module-implementation";
        case D::Unknown:                 return "unknown";
    }
    return "unknown";
}

std::string_view stdlib_name(std::string_view stdlibId) {
    if (stdlibId == "libstdc++") return "libstdc++";
    if (stdlibId == "libc++")    return "libc++";
    if (stdlibId.starts_with("msvc")) return "msvc-stl";
    return "other";
}

std::string toolchain_id(const mcpp::toolchain::Toolchain& tc,
                         std::string_view compilerTriple) {
    return std::format("{}-{}-{}", tc.compiler_family(), tc.version, compilerTriple);
}

std::vector<std::string> split_command_words(std::string_view s, bool windows) {
    return mcpp::manifest::host_command_words(s, windows);
}

std::optional<Invocation> recover_invocation(const std::vector<std::string>& commands,
                                             const std::filesystem::path& source,
                                             const std::filesystem::path& driver,
                                             const std::filesystem::path& defaultDirectory,
                                             bool windows) {
    const std::string driverText = driver.string();
    for (auto const& command : commands) {
        auto words = split_command_words(command, windows);
        std::vector<std::vector<std::string>> segments(1);
        for (auto& w : words) {
            if (w == "&&") segments.emplace_back();
            else segments.back().push_back(std::move(w));
        }
        std::filesystem::path cwd;
        for (auto& seg : segments) {
            if (seg.empty()) continue;
            if (seg.front() == "cd") {
                std::size_t k = 1;
                if (k < seg.size() && (seg[k] == "/d" || seg[k] == "/D")) ++k;
                if (k < seg.size()) cwd = std::filesystem::path{seg[k]};
                continue;
            }
            std::size_t b = 0;
            if (b < seg.size() && seg[b] == "env") ++b;
            while (b < seg.size() && is_assignment(seg[b])) ++b;
            std::vector<std::string> argv;
            for (std::size_t k = b; k < seg.size(); ++k) {
                switch (redirect_kind(seg[k])) {
                    case Redirect::Attached: continue;
                    case Redirect::Detached: ++k; continue;
                    case Redirect::None:     argv.push_back(seg[k]);
                }
            }
            if (argv.empty()) continue;
            // A driver path rendered without quotes splits at its spaces.
            if (argv.front() != driverText
                && driverText.find(' ') != std::string::npos) {
                std::string joined = argv.front();
                std::size_t k = 1;
                while (k < argv.size() && joined.size() < driverText.size()) {
                    joined += ' ';
                    joined += argv[k];
                    ++k;
                }
                if (joined == driverText) {
                    argv.erase(argv.begin() + 1, argv.begin() + static_cast<std::ptrdiff_t>(k));
                    argv.front() = driverText;
                }
            }
            const bool named = std::ranges::any_of(argv, [&](const std::string& w) {
                return names_path(w, source, cwd.empty() ? defaultDirectory : cwd);
            });
            if (!named) continue;
            return Invocation{cwd.empty() ? defaultDirectory : cwd, std::move(argv)};
        }
    }
    return std::nullopt;
}

Rendered render(std::span<const Member> members,
                const std::filesystem::path& workspaceRoot,
                std::string_view selector) {
    Rendered r;
    const bool windows = mcpp::platform::is_windows;
    nlohmann::json toolchains = nlohmann::json::object();
    nlohmann::json sets = nlohmann::json::array();

    std::set<std::string> watch;
    std::set<std::filesystem::path> inputFiles;

    auto watch_file = [&](const std::filesystem::path& p) {
        const auto normal = p.lexically_normal();
        std::error_code ec;
        if (std::filesystem::is_regular_file(normal, ec)) inputFiles.insert(normal);
        if (auto rel = relative_to(normal, workspaceRoot); rel && !rel->empty())
            watch.insert(*rel);
        else
            watch.insert(native_string(normal));
    };
    auto watch_glob = [&](const std::filesystem::path& root, std::string_view glob) {
        if (glob.empty() || glob.starts_with('!')) return;
        const auto matches = mcpp::modgraph::expand_glob(root, glob);
        for (auto const& f : matches) inputFiles.insert(f.lexically_normal());
        if (auto rel = relative_to(root, workspaceRoot)) {
            watch.insert(rel->empty() ? std::string(glob)
                                      : *rel + "/" + std::string(glob));
        } else {
            for (auto const& f : matches) watch.insert(native_string(f.lexically_normal()));
        }
    };

    watch_file(workspaceRoot / "mcpp.toml");
    watch_file(mcpp::home::root() / "config.toml");

    for (auto const& member : members) {
        const auto& ctx = *member.ctx;
        // The triple as the compiler spells it (`x86_64-pc-windows-msvc`),
        // which is what S1 asks for; mcpp's own spelling when none was resolved.
        const std::string compilerTriple = ctx.plan.targetSide.llvmTriple.empty()
            ? ctx.tc.targetTriple : ctx.plan.targetSide.llvmTriple;
        const auto tcId = toolchain_id(ctx.tc, compilerTriple);

        const auto rootName = qualified_name(ctx.manifest);
        std::set<std::filesystem::path> testSources;
        for (auto const& t : ctx.manifest.targets) {
            if (t.kind != mcpp::manifest::Target::TestBinary || t.main.empty()) continue;
            std::filesystem::path main{t.main};
            testSources.insert((main.is_absolute() ? main : ctx.projectRoot / main)
                                   .lexically_normal());
        }

        std::vector<std::string> order;
        std::map<std::string, SetData> groups;
        auto set_for = [&](const std::string& name, const std::string& family,
                           const std::string& kind) -> SetData& {
            auto [it, inserted] = groups.try_emplace(name);
            if (inserted) {
                order.push_back(name);
                it->second.familyName = family;
                it->second.kind = kind;
            }
            return it->second;
        };

        const auto flags = mcpp::build::compute_flags(ctx.plan);
        auto invocations = mcpp::build::unit_invocations(ctx.plan, flags);
        if (!toolchains.contains(tcId))
            toolchains[tcId] = toolchain_json(ctx.tc, compilerTriple, invocations);
        for (auto& inv : invocations) {
            const auto& cu = *inv.unit;
            const bool isTest = testSources.contains(cu.source.lexically_normal());
            const std::string package = cu.packageName.empty() ? rootName : cu.packageName;
            const std::string kind = isTest ? "test"
                                   : package == rootName ? target_kind(ctx.manifest)
                                   : "library";
            auto& set = set_for(member.setPrefix + package + (isTest ? ":test" : ""),
                                package, kind);
            nlohmann::json provides = nlohmann::json::object();
            if (cu.providesModule) provides[*cu.providesModule] = "";
            nlohmann::json requires_ = nlohmann::json::array();
            for (auto const& name : cu.imports) requires_.push_back(name);
            r.compileCommands.push_back(nlohmann::json{
                {"directory", inv.directory},
                {"file",      inv.file},
                {"arguments", inv.arguments},
                {"output",    inv.output},
            });
            set.units.push_back(nlohmann::json{
                {"source",         std::move(inv.file)},
                {"work-directory", std::move(inv.directory)},
                {"arguments",      std::move(inv.arguments)},
                {"object",         std::move(inv.output)},
                {"private",        false},
                {"provides",       std::move(provides)},
                {"requires",       std::move(requires_)},
                {"ide",            {{"role", std::string(role_name(cu.declaration))}}},
            });
        }

        if (ctx.stdModule) {
            const auto& sm = *ctx.stdModule;
            auto add_std = [&](const std::filesystem::path& source,
                               const std::vector<std::string>& commands,
                               const std::filesystem::path& object,
                               std::string_view module,
                               std::vector<std::string> requires_) {
                if (source.empty() || commands.empty()) return;
                auto inv = recover_invocation(commands, source, ctx.tc.binaryPath,
                                              sm.cacheDir, windows);
                if (!inv) {
                    r.notes.push_back({"MCPP_BUILD_DATABASE_STD_UNIT_UNDESCRIBED",
                        std::format("no command that builds the {} module names its "
                                    "source '{}'; the unit is not listed",
                                    module, source.string())});
                    return;
                }
                auto& set = set_for(member.setPrefix + std::string(kStdSetName),
                                    std::string(kStdSetName), "library");
                set.units.push_back(nlohmann::json{
                    {"source",         native_string(source)},
                    {"work-directory", native_string(inv->workDirectory)},
                    {"arguments",      std::move(inv->arguments)},
                    {"object",         native_string(object)},
                    {"private",        false},
                    {"provides",       {{std::string(module), ""}}},
                    {"requires",       std::move(requires_)},
                    {"ide",            {{"role", "module-interface"}}},
                });
            };
            add_std(ctx.tc.stdModuleSource, sm.stdCommands, sm.objectPath, "std", {});
            add_std(ctx.tc.stdCompatSource, sm.compatCommands, sm.compatObjectPath,
                    "std.compat", {"std"});
        }

        for (auto const& name : order) {
            auto& set = groups.at(name);
            nlohmann::json visible = nlohmann::json::array();
            for (auto const& other : order)
                if (other != name) visible.push_back(other);
            nlohmann::json setJson{
                {"name",              name},
                {"family-name",       set.familyName},
                {"visible-sets",      std::move(visible)},
                {"translation-units", std::move(set.units)},
                {"ide", {
                    {"toolchain",     tcId},
                    {"configuration", ctx.profile},
                    {"kind",          set.kind},
                }},
            };
            split_baseline(setJson);
            sets.push_back(std::move(setJson));
        }

        watch_file(ctx.projectRoot / "mcpp.lock");
        for (auto const& sp : ctx.sourcePackages) {
            watch_file(sp.root / "mcpp.toml");
            std::error_code ec;
            if (std::filesystem::exists(sp.root / "build.mcpp", ec))
                watch_file(sp.root / "build.mcpp");
            for (auto const& g : sp.sources) watch_glob(sp.root, g);
        }
        for (auto const& g : member.testDiscover) watch_glob(member.testRoot, g);
        // Only an editable package's build program inputs can change: a store
        // package's are fixed by the version its manifest and lock name.
        std::set<std::filesystem::path> editableRoots;
        for (auto const& sp : ctx.sourcePackages) editableRoots.insert(sp.root.lexically_normal());
        for (auto const& declared : mcpp::build::declared_program_inputs(member.workDir)) {
            if (!editableRoots.contains(declared.root.lexically_normal())) continue;
            for (auto const& f : declared.files) watch_file(f);
            for (auto const& g : declared.globs) watch_glob(declared.root, g);
        }
        for (auto const& note : ctx.planNotes) r.notes.push_back(note);
    }

    r.database = nlohmann::json{
        {"version",  1},
        {"revision", 0},
        {"ide", {
            {"profile-version", std::string(kProfileVersion)},
            {"generator", {
                {"name",    "mcpp"},
                {"version", std::string(mcpp::toolchain::MCPP_VERSION)},
            }},
            {"toolchains", std::move(toolchains)},
        }},
        {"sets", std::move(sets)},
    };

    r.watch.assign(watch.begin(), watch.end());

    std::string digest = "mcpp-build-database-inputs-v1\x1f";
    digest += mcpp::toolchain::MCPP_VERSION;
    digest += '\x1f';
    digest += selector;
    digest += '\n';
    for (auto const& f : inputFiles) {
        digest += f.generic_string();
        digest += '\x1f';
        digest += mcpp::toolchain::hash_file(f);
        digest += '\n';
    }
    r.inputsFingerprint = "fnv1a:" + mcpp::toolchain::hash_string(digest);
    return r;
}

} // namespace mcpp::build::database
