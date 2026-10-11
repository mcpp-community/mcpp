// host_graph.cpp -- the build programs' graph (mcpp#797). Declared in `:state`.
//
// A build program is a program of the build-program configuration: its main
// file is `build.mcpp` (or the program a rule feature synthesises), and its
// dependencies are the libraries its package asks for in
// `[build-dependencies]` -- and those a dependency hands on with
// `reexport = true`. The programs of one build that share a host toolchain and
// a C++ standard are the members of one workspace, written to
//
//     <work root>/target/.build-mcpp/host/<group>/
//
// and planned and built by the engine itself, in a sub-build (the same entry
// the host tools use). Each dependency is compiled as the whole package it is
// -- its include directories, defines, features, its own dependencies, its
// implementation units -- and `build.mcpp` is compiled by the same plan, so a
// BMI and the program importing it come from one flag producer by
// construction. The workspace is written to disk so that `mcpp build` in that
// directory reproduces the compile of the build programs.
//
// A PROGRAM IS COMPILED WHEN ITS IDENTITY MOVED. The identity is computed from
// this build's resolution, without planning the sub-build: the engine
// version, the toolchain, the standard, the program's text, and every package
// of its graph -- name, version, source, the features of its build-program
// instance, and the content of a source tree that can change in place
// (`source_digest`). Every program of a build whose identity stands is run, or
// replayed, without the sub-build being planned at all.

module mcpp.build.prepare;
import :state;

import std;
import mcpp.diag;
import mcpp.log;
import mcpp.build.backend;
import mcpp.build.build_program;
import mcpp.build.engine_package;
import mcpp.build.ninja;
import mcpp.build.plan;
import mcpp.build.progress;
import mcpp.build.refusal;
import mcpp.build.provisions;
import mcpp.build.source_digest;
import mcpp.build.tool_store;
import mcpp.libs.json;
import mcpp.manifest;
import mcpp.modgraph.scanner;
import mcpp.platform.fs;
import mcpp.pm.mangle;
import mcpp.toolchain.fingerprint;
import mcpp.toolchain.triple;
import mcpp.ui;
import mcpp.version;

namespace mcpp::build {

namespace fs = std::filesystem;

namespace {

std::string qualified(const mcpp::manifest::Manifest& m) {
    return m.package.namespace_.empty() ? m.package.name
                                        : m.package.namespace_ + "." + m.package.name;
}

const mcpp::manifest::Manifest& manifest_of(const PrepareState& state, std::size_t i) {
    return i == 0 ? *state.m : state.packages[i].manifest;
}

bool imports_engine(std::string_view text);
std::vector<fs::path> build_plugin_units(const mcpp::manifest::Manifest& m, const fs::path& root,
                                         const std::vector<std::string>& features);

// A TOML basic string.
std::string toml_string(std::string_view s) {
    std::string out = "\"";
    for (char c : s) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

// A bare key when it is one, a quoted key otherwise.
std::string key(std::string_view s) {
    const bool bare = !s.empty() && std::ranges::all_of(s, [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_';
    });
    return bare ? std::string(s) : toml_string(s);
}

std::string path_text(const fs::path& p) {
    const auto u = p.lexically_normal().generic_u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

std::string list(const std::vector<std::string>& v) {
    std::string out = "[";
    for (auto const& e : v) {
        if (out.size() > 1) out += ", ";
        out += toml_string(e);
    }
    return out + "]";
}

// The program text of package `i`: its `build.mcpp`, or the program its
// rules describe when it has none.
struct ProgramSource {
    fs::path    file;         // empty when synthesised
    std::string synthesised;  // the text, when synthesised
    bool exists() const { return !file.empty() || !synthesised.empty(); }
};

ProgramSource program_source(const PrepareState& state, std::size_t i) {
    const auto root = i == 0 ? *state.root : state.packages[i].root;
    std::error_code ec;
    if (fs::is_regular_file(root / "build.mcpp", ec)) return {root / "build.mcpp", {}};
    auto const& rules = manifest_of(state, i).buildConfig.ruleModules;
    if (rules.empty()) return {};
    return {{}, mcpp::build::synthesised_rule_program(rules)};
}

// The member a program is in the build programs' workspace: its package's
// qualified name, made a directory name, and a digest of its root (two
// packages of one name do not meet in one build, but a name says nothing
// about where it came from).
std::string member_name(const PrepareState& state, std::size_t i) {
    std::string n = qualified(manifest_of(state, i));
    for (auto& c : n)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') c = '-';
    const auto root = i == 0 ? *state.root : state.packages[i].root;
    return std::format("{}-{}", n, mcpp::toolchain::hash_string(path_text(root)).substr(0, 6));
}

// The libraries program `i` uses: the providers of a build-program library
// this package sees, its own `[build-dependencies]` and what a dependency
// re-exports to it.
std::vector<std::size_t> program_libraries(const PrepareState& state, std::size_t i) {
    std::vector<std::size_t> out;
    if (i < state.provisionGraph.visible.size())
        for (auto const& pr : state.provisionGraph.visible[i])
            if (pr.kind == mcpp::build::provisions::Kind::HostModule
                && pr.provider < state.packages.size()
                && std::ranges::find(out, pr.provider) == out.end())
                out.push_back(pr.provider);
    std::ranges::sort(out);
    return out;
}

// Every package below `roots` in this build's graph, over every edge: the
// packages the build programs' graph will contain.
std::vector<std::size_t> closure_of(const PrepareState& state, std::vector<std::size_t> roots) {
    std::set<std::size_t> seen(roots.begin(), roots.end());
    while (!roots.empty()) {
        const auto p = roots.back();
        roots.pop_back();
        for (auto const& e : state.dependencyEdges)
            if (e.consumerPackageIndex == p && e.dependencyPackageIndex < state.packages.size()
                && seen.insert(e.dependencyPackageIndex).second)
                roots.push_back(e.dependencyPackageIndex);
    }
    return {seen.begin(), seen.end()};
}

// How the build programs' graph names package `d`: the version this build
// resolved for an index package, the directory otherwise (a `path` package,
// a git checkout, a workspace member).
struct DependencyLine {
    std::string ns, name, source;   // `source`: the inline table's location keys
};

DependencyLine dependency_line(const PrepareState& state, std::size_t d) {
    auto const& m = state.packages[d].manifest;
    DependencyLine line{m.package.namespace_, m.package.name, {}};
    const auto* ident = d >= 1 && d - 1 < state.dep_cache_identities.size()
        ? &state.dep_cache_identities[d - 1] : nullptr;
    if (ident && ident->sourceKind == "version" && !engine::is_engine_package(state.packages[d].root)) {
        line.source = std::format("version = {}", toml_string("=" + (ident->version.empty()
                                                                ? m.package.version
                                                                : ident->version)));
        if (!ident->packageName.empty()) {
            // The index address the build resolved, which a Form B package's
            // manifest may spell differently.
            const auto dot = ident->packageName.find('.');
            if (line.ns.empty() && dot != std::string::npos) {
                line.ns = ident->packageName.substr(0, dot);
                line.name = ident->packageName.substr(dot + 1);
            }
        }
    } else {
        line.source = std::format("path = {}", toml_string(path_text(state.packages[d].root)));
    }
    return line;
}

std::string features_of(const PrepareState& state, std::size_t d) {
    const auto& f = d < state.buildProgramFeaturesByPackage.size()
        ? state.buildProgramFeaturesByPackage[d] : std::vector<std::string>{};
    std::vector<std::string> named;
    for (auto const& x : f)
        if (x != "default") named.push_back(x);
    return std::format("features = {}, default-features = {}", list(named),
                       std::ranges::find(f, std::string("default")) != f.end() ? "true" : "false");
}

// The toolchain the build programs are compiled with, as a sub-build is told
// it (the host tools' rule, SPEC-004 §10.1), and its identity.
struct HostToolchain {
    std::string spec;
    std::string identity;
};

std::expected<HostToolchain, std::string> host_toolchain(PrepareState& state) {
    auto host = state.host_tc_for_build_program();
    if (!host) return std::unexpected(host.error());
    HostToolchain t;
    t.spec = state.host_spec_for_build_program();
    t.identity = std::format("{}|{}|{}|{}", t.spec, path_text(host->first),
                             host->second.version, host->second.targetTriple);
    return t;
}

fs::path host_root(const PrepareState& state) {
    return state.workRoot / "target" / ".build-mcpp" / "host";
}

std::string standard_of(const PrepareState& state, std::size_t i) {
    auto const& s = manifest_of(state, i).cppStandard.canonical;
    return s.empty() ? std::string("c++23") : s;
}

// The identity of program `i`: what its compile and link read.
std::string program_identity(PrepareState& state, std::size_t i, const ProgramSource& src,
                             const HostToolchain& tc) {
    std::string text = std::format("mcpp-build-program-v1\nengine {}\ntoolchain {}\nstandard {}\n",
                                   mcpp::MCPP_VERSION, tc.identity, standard_of(state, i));
    text += src.file.empty() ? "program synthesised " + mcpp::toolchain::hash_string(src.synthesised)
                             : "program " + mcpp::toolchain::hash_file(src.file);
    text += '\n';
    const auto libs = program_libraries(state, i);
    for (auto d : libs) text += std::format("uses {}\n", qualified(state.packages[d].manifest));
    for (auto d : closure_of(state, libs)) {
        auto const& pkg = state.packages[d];
        const auto* ident = d >= 1 && d - 1 < state.dep_cache_identities.size()
            ? &state.dep_cache_identities[d - 1] : nullptr;
        std::string source = ident ? ident->sourceKind + ":" + ident->sourceRef : "";
        std::string tree;
        // A tree that can change in place: anything but an index package or
        // a pinned git checkout.
        if (!ident || (ident->sourceKind != "version" && ident->sourceKind != "git")) {
            const auto cache = host_root(state) / "digests"
                             / (mcpp::toolchain::hash_string(path_text(pkg.root)) + ".tsv");
            tree = mcpp::build::source_tree_digest(pkg.root, cache);
        }
        const auto& f = d < state.buildProgramFeaturesByPackage.size()
            ? state.buildProgramFeaturesByPackage[d] : std::vector<std::string>{};
        text += std::format("package {} {} {} [{}] {}\n", qualified(pkg.manifest),
                            ident ? ident->version : pkg.manifest.package.version, source,
                            list(f), tree);
    }
    return mcpp::toolchain::hash_string(text);
}

// The primary modules package `d` exports in its build-program instance: the
// units its sources select, with the sources of the features that instance
// has and the target instance's manifest may not have folded in.
std::set<std::string> exported_modules(const PrepareState& state, std::size_t d,
                                       const std::vector<std::string>& features) {
    auto const& pkg = state.packages[d];
    std::set<fs::path> files;
    for (auto const& f : mcpp::modgraph::package_source_files(pkg.root, pkg.manifest))
        files.insert(f);
    for (auto const& f : features)
        if (auto it = pkg.manifest.buildConfig.featureSources.find(f);
            it != pkg.manifest.buildConfig.featureSources.end())
            for (auto const& g : it->second)
                for (auto const& file : mcpp::modgraph::expand_glob(pkg.root, g))
                    files.insert(file);
    std::set<std::string> out;
    for (auto const& file : files) {
        std::ifstream in(file, std::ios::binary);
        const std::string text{std::istreambuf_iterator<char>(in), {}};
        for (auto const& m : mcpp::pm::declared_module_roots(text)) out.insert(m);
    }
    return out;
}

const std::vector<std::string>& program_features(const PrepareState& state, std::size_t d) {
    static const std::vector<std::string> none;
    return d < state.buildProgramFeaturesByPackage.size() ? state.buildProgramFeaturesByPackage[d]
                                                          : none;
}

// What a program may import, refused by name before anything is compiled: a
// module of a package that reaches the program only as another library's
// dependency, a module a library offers behind a feature that is not enabled
// (SPEC-007 R9.7), and a module no library provides. Left to the compiler,
// each is `failed to read compiled module`, naming neither the package nor
// the key that would change the answer.
std::expected<void, std::string> check_program_imports(PrepareState& state, std::size_t i,
                                                       const ProgramSource& src) {
    std::string text = src.synthesised;
    if (!src.file.empty()) {
        std::ifstream in(src.file, std::ios::binary);
        text.assign(std::istreambuf_iterator<char>(in), {});
    }
    const auto wants = mcpp::pm::imported_module_names(text);
    if (wants.empty()) return {};
    const auto libs = program_libraries(state, i);
    std::set<std::string> available{"std", "std.compat", "mcpp", "mcpp.core"};
    std::map<std::string, std::size_t> providerOf;
    for (auto d : libs)
        for (auto const& m : exported_modules(state, d, program_features(state, d))) {
            available.insert(m);
            providerOf.emplace(m, d);
        }
    std::set<std::string> prerequisites;
    for (auto d : closure_of(state, libs))
        if (std::ranges::find(libs, d) == libs.end())
            for (auto const& m : exported_modules(state, d, program_features(state, d)))
                prerequisites.insert(m);

    auto const& m = manifest_of(state, i);
    for (auto const& want : wants) {
        if (available.contains(want)) continue;
        if (prerequisites.contains(want)) {
            refusal::record(refusal::Code::HostModuleMissing);
            return std::unexpected(std::format(
                "build.mcpp imports '{}', which reaches this build only as a "
                "dependency of another build dependency.\n"
                "       A library's own dependencies are not importable from the "
                "program unless that library hands them on.\n"
                "       Either depend on the package providing '{}' directly, or "
                "have the library that owns it re-export it:\n"
                "         <that library's package> = {{ ..., reexport = true }}",
                want, want));
        }
        // #734 E7: a module a library offers behind a feature this build has
        // not enabled. Its files are read only now, on the way to an error.
        for (auto d : libs) {
            auto const& pm = state.packages[d].manifest;
            const auto& active = program_features(state, d);
            for (auto const& [feature, globs] : pm.buildConfig.featureSources) {
                if (std::ranges::find(active, feature) != active.end()) continue;
                for (auto const& g : globs)
                    for (auto const& file : mcpp::modgraph::expand_glob(state.packages[d].root, g)) {
                        std::ifstream in(file, std::ios::binary);
                        const std::string t{std::istreambuf_iterator<char>(in), {}};
                        const auto decl = mcpp::pm::declared_module_roots(t);
                        if (std::ranges::find(decl, want) == decl.end()) continue;
                        refusal::record(refusal::Code::HostModuleMissing);
                        return std::unexpected(std::format(
                            "build.mcpp imports '{}'\n"
                            "  provided by: {}, feature \"{}\" (not enabled)\n"
                            "  hint: enable it on the dependency edge: {} = {{ ..., "
                            "features = [\"{}\"] }}",
                            want, qualified(pm), feature, qualified(pm), feature));
                    }
            }
        }
        // A dependency that could plausibly be meant: one the program does not
        // use as a library. Ranked by the one relation available without
        // resolving anything: a key sharing the imported name's first segment.
        const auto dot  = want.find('.');
        const auto head = dot == std::string::npos ? want : want.substr(0, dot);
        std::string near, all;
        auto scan = [&](const std::map<std::string, mcpp::manifest::DependencySpec>& deps,
                        std::string_view section, bool buildTable) {
            for (auto const& [k, s] : deps) {
                const bool library = buildTable
                    && (s.hostModuleDeclared ? s.hostModule : s.tools.empty() && s.artifacts.empty());
                if (library) continue;
                const auto one = std::format("{} (in [{}])", k, section);
                auto& dst = k.starts_with(head + ".") || k == head ? near : all;
                if (!dst.empty()) dst += ", ";
                dst += one;
            }
        };
        scan(m.buildDependencies, "build-dependencies", true);
        scan(m.dependencies, "dependencies", false);
        const std::string candidates = near.empty() ? all : near;
        std::string importable;
        for (auto const& a : available) {
            if (a == "std" || a == "std.compat" || a == "mcpp" || a == "mcpp.core") continue;
            if (!importable.empty()) importable += ", ";
            importable += a;
        }
        refusal::record(refusal::Code::HostModuleMissing);
        return std::unexpected(std::format(
            "build.mcpp imports '{}', and no build dependency provides it.\n"
            "       A build program imports the public modules of the libraries in its\n"
            "       package's [build-dependencies]; an entry that lists only `tools`, or\n"
            "       states `host-module = false`, asks for no library:\n"
            "         [build-dependencies.<namespace>]\n"
            "         <name> = {{ version = \"...\" }}      # or `host-module = true` beside `tools`\n"
            "       importable here: {}\n"
            "{}",
            want, importable.empty() ? "(none)" : importable,
            candidates.empty()
                ? "       This package declares no dependency that could provide it."
                : std::format("       declared without its library: {}", candidates)));
    }
    return {};
}

// ONE MODULE NAME, ONE PROVIDER, IN ONE PROGRAM. Two packages of a program's
// graph that declare one module name would both be compiled and linked, and
// the failure would be a duplicate module initializer naming one object twice.
// The two package identities and the two interface paths are the content of
// the refusal: with one name shared they are the only way to tell them apart.
std::expected<void, std::string> check_module_collisions(const PrepareState& state, std::size_t i) {
    const auto libs = program_libraries(state, i);
    std::map<std::string, std::pair<std::size_t, fs::path>> seen;
    for (auto d : closure_of(state, libs)) {
        if (engine::is_engine_package(state.packages[d].root)) continue;
        auto const& pkg = state.packages[d];
        std::set<fs::path> files;
        for (auto const& f : mcpp::modgraph::package_source_files(pkg.root, pkg.manifest))
            files.insert(f);
        for (auto const& f : program_features(state, d))
            if (auto it = pkg.manifest.buildConfig.featureSources.find(f);
                it != pkg.manifest.buildConfig.featureSources.end())
                for (auto const& g : it->second)
                    for (auto const& file : mcpp::modgraph::expand_glob(pkg.root, g))
                        files.insert(file);
        for (auto const& file : files) {
            std::ifstream in(file, std::ios::binary);
            const std::string text{std::istreambuf_iterator<char>(in), {}};
            for (auto const& m : mcpp::pm::declared_module_roots(text)) {
                auto [it, fresh] = seen.try_emplace(m, d, file);
                if (fresh || it->second.first == d) continue;
                return std::unexpected(std::format(
                    "two libraries of the build program of '{}' declare the module '{}':\n"
                    "         {}  ({})\n"
                    "         {}  ({})\n"
                    "       A module name identifies one module within a program. Depend on one "
                    "of them, or rename the module in one.",
                    qualified(manifest_of(state, i)), m,
                    qualified(state.packages[it->second.first].manifest),
                    path_text(it->second.second), qualified(pkg.manifest), path_text(file)));
            }
        }
    }
    return {};
}

// SPEC-007 R9.6: a module whose name opens with `mcpp.<reserved>` comes only
// from a package of namespace `mcpp`. Warned once per (package, module).
void warn_reserved_prefixes(const PrepareState& state, std::size_t i) {
    static std::set<std::string> warned;
    for (auto d : program_libraries(state, i)) {
        auto const& pm = state.packages[d].manifest;
        for (auto const& m : exported_modules(state, d, program_features(state, d)))
            if (auto w = mcpp::build::provisions::reserved_prefix_warning(m, pm.package.namespace_,
                                                                          qualified(pm)))
                if (warned.insert(qualified(pm) + "\x1e" + m).second)
                    mcpp::diag::warning("build/rule-namespace", *w);
    }
}

// What `ensure_build_programs` learned about one program.
struct Program {
    std::size_t   index = 0;
    ProgramSource source;
    std::string   member;
    std::string   identity;
    std::string   group;      // the group directory's name
};

// The record of the programs a group directory holds.
nlohmann::json read_record(const fs::path& file) {
    std::ifstream in(file);
    auto j = in ? nlohmann::json::parse(in, nullptr, false) : nlohmann::json{};
    return j.is_object() ? j : nlohmann::json::object();
}

void write_if_changed(const fs::path& file, const std::string& text) {
    std::error_code ec;
    {
        std::ifstream in(file, std::ios::binary);
        if (in && std::string{std::istreambuf_iterator<char>(in), {}} == text) return;
    }
    fs::create_directories(file.parent_path(), ec);
    const auto tmp = file.string() + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << text;
    }
    fs::rename(tmp, file, ec);
}

// The manifests of one group: the workspace and a member per program.
std::map<std::string, std::string> resolution_pins(const PrepareState& state);

void write_group(PrepareState& state, const fs::path& dir, const std::vector<Program>& programs,
                 const std::string& standard, const fs::path& enginePackage,
                 const std::string& toolchainSpec) {
    std::string root = std::format(
        "# Written by mcpp {}: the build programs of {} (mcpp#797).\n"
        "# `mcpp build` here reproduces their compile; it is rewritten on every build.\n"
        "[workspace]\nmembers = [",
        mcpp::MCPP_VERSION, path_text(state.workRoot));
    for (std::size_t k = 0; k < programs.size(); ++k)
        root += (k ? ", " : "") + toml_string(programs[k].member);
    // The engine that wrote it is its floor: the workspace layer below is read
    // by 2026.10.10.1 and later.
    root += std::format("]\n\n[workspace.package]\nmcpp = \">={}\"\n", mcpp::MCPP_VERSION);
    root += "\n[workspace.profile.build-program]\nopt = 0\ndebug = false\n";
    if (!state.m->indices.empty()) {
        root += "\n[workspace.indices]\n";
        for (auto const& [name, idx] : state.m->indices) {
            std::vector<std::string> fields;
            if (idx.is_local()) fields.push_back("path = " + toml_string(path_text(idx.path)));
            if (!idx.url.empty()) fields.push_back("url = " + toml_string(idx.url));
            if (!idx.rev.empty()) fields.push_back("rev = " + toml_string(idx.rev));
            if (!idx.tag.empty()) fields.push_back("tag = " + toml_string(idx.tag));
            if (!idx.branch.empty()) fields.push_back("branch = " + toml_string(idx.branch));
            if (!idx.artifact.empty()) fields.push_back("artifact = " + toml_string(idx.artifact));
            if (!idx.source.empty()) fields.push_back("source = " + toml_string(idx.source));
            std::string row;
            for (auto const& f : fields) row += (row.empty() ? "" : ", ") + f;
            root += std::format("{} = {{ {} }}\n", key(name.empty() ? "default" : name), row);
        }
    }
    // Where a payload comes from is the build root's statement (mcpp#755); the
    // packages of the build programs' graph declare payloads too, and are
    // provisioned by the sub-build under the same statement. A relative path
    // is relative to the manifest that wrote it.
    {
        std::map<std::string, std::string> rows;
        const mcpp::manifest::Manifest* stating[] = {state.m ? &*state.m : nullptr,
                                                     state.runtimeOwnerManifest};
        for (auto const* man : stating) {
            if (!man) continue;
            for (auto const& [k, o] : man->xlings.overrides) {
                if (rows.contains(k)) continue;
                using Kind = mcpp::manifest::XlingsOverride::Kind;
                auto value = o.value;
                if (fs::path v(value); (o.kind != Kind::Program || names_a_file_path(value))
                                           && v.is_relative())
                    value = path_text((man->sourcePath.parent_path() / v).lexically_normal());
                const bool isRoot = o.kind == Kind::Root
                    || (o.kind == Kind::Path && fs::is_directory(value));
                std::vector<std::string> fields;
                fields.push_back(std::format("{} = {}", isRoot ? "root" : "program", toml_string(value)));
                if (!o.version.empty()) fields.push_back("version = " + toml_string(o.version));
                std::string row;
                for (auto const& f : fields) row += (row.empty() ? "" : ", ") + f;
                rows.emplace(k, std::format("{} = {{ {} }}\n", toml_string(k), row));
            }
        }
        if (!rows.empty()) {
            root += "\n[workspace.xlings.overrides]\n";
            for (auto const& [k, row] : rows) root += row;
        }
    }
    write_if_changed(dir / "mcpp.toml", root);
    // What the sub-build is given, for a command run in this directory
    // (driver.cpp, `read_build_programs_marker`).
    {
        std::string marker = std::format("toolchain={}\nprofile=build-program\nindex-store={}\n",
                                         toolchainSpec, path_text(state.projectIndexRoot()));
        for (auto const& [k, v] : resolution_pins(state)) marker += std::format("pin={}={}\n", k, v);
        write_if_changed(dir / ".mcpp-build-programs", marker);
    }

    for (auto const& p : programs) {
        const auto memberDir = dir / p.member;
        fs::path main = p.source.file;
        if (main.empty()) {
            // Not `build.mcpp` at the member's root: that file would be the
            // member's OWN build program.
            main = memberDir / "synthesised.mcpp";
            write_if_changed(main, p.source.synthesised);
        }
        std::string m = std::format(
            "# The build program of {} ({}).\n"
            "[package]\nname = {}\nversion = \"0.0.0\"\nstandard = {}\n\n"
            "[build]\nsources = []\n\n",
            qualified(manifest_of(state, p.index)), path_text(main), toml_string(p.member),
            toml_string(standard));
        std::map<std::string, std::vector<std::string>> tables;   // namespace -> lines
        // THE ENGINE INTERFACE ONLY WHERE SOMETHING IMPORTS IT. A module's
        // object is linked whether or not anything references it, and the
        // interface announces the directive protocol when the program starts:
        // a hand-written `printf` program that imports nothing must stay one,
        // whose unknown directives are warned about rather than refused.
        bool needsEngine = !p.source.synthesised.empty();
        if (!needsEngine) {
            std::ifstream in(p.source.file, std::ios::binary);
            needsEngine = imports_engine(std::string{std::istreambuf_iterator<char>(in), {}});
        }
        if (!needsEngine)
            for (auto d : closure_of(state, program_libraries(state, p.index)))
                if (!build_plugin_units(state.packages[d].manifest, state.packages[d].root,
                                        program_features(state, d)).empty()) {
                    needsEngine = true;
                    break;
                }
        if (needsEngine)
            tables[std::string(engine::kNamespace)].push_back(
                std::format("{} = {{ path = {} }}", key(engine::kName), toml_string(path_text(enginePackage))));
        for (auto d : program_libraries(state, p.index)) {
            const auto line = dependency_line(state, d);
            tables[line.ns].push_back(std::format("{} = {{ {}, {} }}", key(line.name),
                                                  line.source, features_of(state, d)));
        }
        if (auto it = tables.find(""); it != tables.end()) {
            m += "[dependencies]\n";
            for (auto const& l : it->second) m += l + "\n";
            m += "\n";
        }
        for (auto const& [ns, lines] : tables) {
            if (ns.empty()) continue;
            m += std::format("[dependencies.{}]\n", key(ns));
            for (auto const& l : lines) m += l + "\n";
            m += "\n";
        }
        m += std::format("[targets.build-program]\nkind = \"bin\"\nmain = {}\n"
                         "windows_code_page = \"utf-8\"\n", toml_string(path_text(main)));
        write_if_changed(memberDir / "mcpp.toml", m);
    }
}

// The packages of this build that the build programs' graph shares, at the
// version this build resolved.
std::map<std::string, std::string> resolution_pins(const PrepareState& state) {
    std::map<std::string, std::string> pins;
    for (auto const& [k, rec] : state.resolved) {
        if (rec.depIndex >= state.dep_cache_identities.size()) continue;
        auto const& ident = state.dep_cache_identities[rec.depIndex];
        if (ident.sourceKind != "version" || rec.version.empty()) continue;
        pins[k.ns.empty() ? k.shortName : k.ns + "." + k.shortName] = rec.version;
    }
    return pins;
}

// Build the group's programs `goals`: one sub-build, one ninja.
std::expected<std::map<std::string, fs::path>, std::string>
build_group(PrepareState& state, const fs::path& dir, const std::vector<Program>& all,
            const std::vector<const Program*>& goals, const HostToolchain& tc) {
    // The graph `mcpp emit build-database` describes, beside the one it builds.
    auto describe = [&](const BuildContext& ctx) {
        if (!state.overrides.plan_only) return;
        BuildProgramGraph g;
        g.ctx = std::make_shared<const BuildContext>(ctx);
        g.dir = dir;
        for (auto const& p : all) g.programOf[p.member] = qualified(manifest_of(state, p.index));
        state.buildProgramGraphs.push_back(std::move(g));
    };
    BuildOverrides sub;
    sub.project_root = dir;
    sub.work_dir     = dir;
    sub.target_triple = "";
    sub.toolchain    = tc.spec;
    sub.profile      = "build-program";
    sub.cache_mode   = state.overrides.cache_mode;
    sub.strict       = state.overrides.strict;
    sub.tool_depth   = state.overrides.tool_depth + 1;
    sub.tool_chain_sources = state.overrides.tool_chain_sources;
    sub.tool_chain   = state.overrides.tool_chain.empty()
        ? std::string("root → build programs")
        : state.overrides.tool_chain + " → build programs";
    sub.inherited_runtime_selection =
        std::make_shared<const mcpp::xlings::runtime::RuntimeSelection>(state.runtimeSelection);
    sub.inherited_runtime_binding =
        std::make_shared<const mcpp::platform::runtime::RuntimeBinding>(state.runtimeBindingSnapshot);
    sub.resolution_pins = resolution_pins(state);
    sub.build_program_graph = true;
    sub.project_index_root = state.projectIndexRoot();
    for (auto const& p : all) sub.workspace_members.push_back(p.member);

    auto reproduce = [&] {
        return std::format("\n  the build programs' workspace: {}\n"
                           "  re-run it directly:  cd {} && mcpp build\n"
                           "  (MCPP_TOOL_BUILD_VERBOSE=1 shows the inner build's output unfiltered)",
                           path_text(dir), path_text(dir));
    };
    if (state.overrides.tool_depth >= mcpp::build::tool_store::kMaxDepth)
        return std::unexpected(std::format(
            "build programs nested more than {} levels deep — a build program's library "
            "is its own build program's library.\n  chain: {}",
            mcpp::build::tool_store::kMaxDepth, sub.tool_chain));

    mcpp::log::ScopedTimer timer("host-graph", std::format("build programs in {}", path_text(dir)));
    // THE SUB-BUILD SAYS NOTHING ON THE TERMINAL. Its toolchain, its members
    // and its compiles are the build programs' line ("build.mcpp <pkg>
    // compiled · ran"); what it would narrate is in the log, and a failure
    // comes back as this function's error.
    struct Quiet {
        bool prev = mcpp::ui::is_quiet();
        Quiet() { mcpp::ui::set_quiet(true); }
        ~Quiet() { mcpp::ui::set_quiet(prev); }
    } quiet;
    auto ctx = prepare_build(/*print_fingerprint=*/false, /*includeDevDeps=*/false, {}, sub);
    if (!ctx)
        return std::unexpected(std::format("planning the build programs failed: {}{}",
                                           ctx.error(), reproduce()));
    timer.mark("planned");
    describe(*ctx);
    if (goals.empty()) return std::map<std::string, fs::path>{};

    std::map<std::string, fs::path> binaries;
    std::vector<std::string> ninjaGoals;
    for (auto const& lu : ctx->plan.linkUnits) {
        if (lu.kind != mcpp::build::LinkUnit::Binary || lu.targetName != "build-program") continue;
        for (auto const* g : goals)
            if (lu.memberOf == g->member) {
                binaries[g->member] = ctx->plan.outputDir / lu.output;
                ninjaGoals.push_back(lu.output.generic_string());
            }
    }
    for (auto const* g : goals)
        if (!binaries.contains(g->member))
            return std::unexpected(std::format(
                "the build programs' plan has no program for '{}'{}",
                qualified(manifest_of(state, g->index)), reproduce()));

    auto be = mcpp::build::make_ninja_backend();
    mcpp::build::BuildOptions bopt;
    bopt.ninjaTargets = ninjaGoals;
    bopt.report = mcpp::build::BuildOptions::Report::Caller;
    if (const char* v = std::getenv("MCPP_TOOL_BUILD_VERBOSE");
        v && *v && std::string_view(v) != "0")
        bopt.verbose = true;
    auto br = be->build(ctx->plan, bopt);
    timer.mark("built");
    // What an index package and the engine interface compiled to is kept in
    // the global dependency cache, as for any build (`--cache` decides).
    if (br && br->exitCode == 0) mcpp::build::populate_dependency_cache(*ctx);
    if (!br || br->exitCode != 0) {
        std::string who;
        for (auto const* g : goals)
            who += (who.empty() ? "" : ", ") + qualified(manifest_of(state, g->index));
        const auto diag = br ? std::string{} : br.error().diagnosticOutput;
        const auto msg  = br ? std::format("exit {}", br->exitCode) : br.error().message;
        return std::unexpected(std::format("build.mcpp of {} failed to compile ({}){}\n{}",
                                           who, msg, reproduce(), diag));
    }
    return binaries;
}

// ── Build plugins (mcpp#797, SPEC-007 P1-P5) ──────────────────────────────
//
// A unit that imports the engine interface (`mcpp.core`, or `mcpp`) is a
// build-plugin unit: the interface exists only in the build-program
// configuration. Nothing declares it; the import is the fact.

bool imports_engine(std::string_view text) {
    for (auto const& m : mcpp::pm::imported_module_names(text))
        if (m == "mcpp" || m == "mcpp.core") return true;
    return false;
}

std::vector<fs::path> build_plugin_units(const mcpp::manifest::Manifest& m, const fs::path& root,
                                         const std::vector<std::string>& features) {
    std::set<fs::path> files;
    for (auto const& f : mcpp::modgraph::package_source_files(root, m)) files.insert(f);
    for (auto const& f : features)
        if (auto it = m.buildConfig.featureSources.find(f); it != m.buildConfig.featureSources.end())
            for (auto const& g : it->second)
                for (auto const& file : mcpp::modgraph::expand_glob(root, g)) files.insert(file);
    std::vector<fs::path> units;
    bool declaresEngine = false;
    for (auto const& file : files) {
        std::ifstream in(file, std::ios::binary);
        const std::string text{std::istreambuf_iterator<char>(in), {}};
        for (auto const& d : mcpp::pm::declared_module_roots(text))
            if (d == "mcpp" || d == "mcpp.core") declaresEngine = true;
        if (imports_engine(text)) units.push_back(file);
    }
    // A package that is itself named `mcpp` provides that module; its units
    // import their own package.
    if (declaresEngine) units.clear();
    return units;
}

}  // namespace

// P3: a package whose LIBRARY has build-plugin units, built as the root of a
// build (`mcpp build`, `mcpp test`, a selected member), is planned in the
// build-program configuration: for this machine, with the engine interface a
// dependency of it. The library's own `[dependencies]` are then build-program
// instances, which is what a plugin built on other plugins needs.
std::expected<void, std::string>
configure_build_plugin_root(PrepareState& state, mcpp::manifest::Manifest& m,
                            const fs::path& root, const std::vector<std::string>& features) {
    if (state.overrides.build_program_graph) return {};
    if (!mcpp::manifest::has_lib_target(m)) return {};
    const auto units = build_plugin_units(m, root, features);
    if (units.empty()) return {};
    const auto who = qualified(m);
    if (!state.overrides.target_triple.empty()) {
        const auto want = mcpp::toolchain::triple::parse(state.overrides.target_triple);
        const auto host = mcpp::toolchain::triple::host_triple();
        if (!want || want->arch != host.arch || want->os != host.os)
            return std::unexpected(std::format(
                "'{}' is a build plugin: '{}' imports mcpp.core, which exists only in a build\n"
                "       program, and a build program runs on the machine doing the build. It is\n"
                "       built for this machine; --target {} cannot apply to it.",
                who, path_text(units.front()), state.overrides.target_triple));
    }
    auto engineDir = engine::ensure_package();
    if (!engineDir) return std::unexpected(engineDir.error());
    const std::string key = std::format("{}.{}", engine::kNamespace, engine::kName);
    if (!m.dependencies.contains(key)) {
        mcpp::manifest::DependencySpec spec;
        spec.namespace_ = std::string(engine::kNamespace);
        spec.shortName  = std::string(engine::kName);
        spec.path       = path_text(*engineDir);
        spec.declaredIn = "[dependencies]";
        spec.candidates = {{.namespace_ = spec.namespace_, .shortName = spec.shortName}};
        m.dependencies.emplace(key, std::move(spec));
    }
    state.buildPluginRoots.insert(path_text(root));
    mcpp::log::info("build-program", std::format(
        "{}: a build plugin ({} imports mcpp.core); planned in the build-program configuration",
        who, path_text(units.front())));
    mcpp::ui::status("Configuration", std::format("build-program (host {}) for {}",
                                                  mcpp::toolchain::triple::host_triple().str(), who));
    return {};
}

// P1, P4: a build-plugin unit in a target instance. Called after the scan.
std::expected<void, std::string> refuse_plugin_units_in_target(PrepareState& state) {
    if (state.overrides.build_program_graph) return {};
    bool engineInGraph = false;
    for (auto const& pkg : state.packages)
        if (engine::is_engine_package(pkg.root)) engineInGraph = true;
    if (engineInGraph) return {};
    for (auto const& u : state.scan.graph.units) {
        bool importsEngine = false;
        for (auto const& req : u.requires_)
            if (req.logicalName == "mcpp" || req.logicalName == "mcpp.core") importsEngine = true;
        if (!importsEngine) continue;
        std::size_t p = state.packages.size();
        for (std::size_t i = 0; i < state.packages.size(); ++i)
            if (qualified(state.packages[i].manifest) == u.packageName) { p = i; break; }
        if (p == state.packages.size()) continue;
        refusal::record(refusal::Code::HostModuleMissing);
        if (p == 0 || state.packages[p].selectedMember)
            return std::unexpected(std::format(
                "{}: imports mcpp.core, the engine interface, which exists only in a build\n"
                "       program. A project's build program is its `build.mcpp`; a library that\n"
                "       other build programs import is a build plugin, built as such when its\n"
                "       library imports mcpp.core (this unit belongs to a program target).",
                path_text(u.path)));
        // The chain of ordinary edges that brought it into the target.
        std::vector<std::size_t> parent(state.packages.size(), state.packages.size());
        std::vector<std::size_t> work{0};
        std::vector<bool> seen(state.packages.size(), false);
        seen[0] = true;
        while (!work.empty()) {
            const auto c = work.back();
            work.pop_back();
            for (auto const& e : state.dependencyEdges)
                if (e.consumerPackageIndex == c && !e.buildTable
                    && e.dependencyPackageIndex < seen.size() && !seen[e.dependencyPackageIndex]) {
                    seen[e.dependencyPackageIndex] = true;
                    parent[e.dependencyPackageIndex] = c;
                    work.push_back(e.dependencyPackageIndex);
                }
        }
        std::string chain = qualified(state.packages[p].manifest);
        for (auto q = parent[p]; q < state.packages.size(); q = parent[q])
            chain = qualified(manifest_of(state, q)) + " → " + chain;
        const auto consumer = parent[p] < state.packages.size()
            ? qualified(manifest_of(state, parent[p])) : std::string("the project");
        return std::unexpected(std::format(
            "'{}' is a build plugin: {} imports mcpp.core, which exists only in a build program,\n"
            "       and it reaches this target through [dependencies]: {}\n"
            "       A build plugin serves the build program. In {}, write it in\n"
            "       [build-dependencies] instead of [dependencies].",
            qualified(state.packages[p].manifest), path_text(u.path), chain, consumer));
    }
    return {};
}

std::expected<void, std::string>
ensure_build_programs(PrepareState& state, const std::vector<std::size_t>& packages) {
    std::vector<std::size_t> wanted;
    for (auto i : packages)
        if (!state.buildProgramBinaries.contains(i) && program_source(state, i).exists())
            wanted.push_back(i);
    if (wanted.empty()) return {};

    auto tc = host_toolchain(state);
    if (!tc) return std::unexpected(tc.error());
    auto enginePackage = engine::ensure_package();
    if (!enginePackage) return std::unexpected(enginePackage.error());

    // One group per standard: a BMI is used only by a compile that agrees on it.
    std::map<std::string, std::vector<Program>> groups;
    for (auto i : wanted) {
        Program p;
        p.index = i;
        p.source = program_source(state, i);
        p.member = member_name(state, i);
        p.identity = program_identity(state, i, p.source, *tc);
        const auto standard = standard_of(state, i);
        p.group = mcpp::toolchain::hash_string(
            std::format("{}|{}|{}", tc->identity, standard, mcpp::MCPP_VERSION)).substr(0, 12);
        groups[standard].push_back(std::move(p));
    }

    for (auto& [standard, programs] : groups) {
        const auto dir = host_root(state) / programs.front().group;
        std::error_code ec;
        fs::create_directories(dir, ec);
        // Two configuration groups of one command may share a host toolchain
        // and a standard: one builds, the other waits and finds it built.
        std::optional<mcpp::platform::fs::FileLock> lock;
        for (int waited = 0; !(lock = mcpp::platform::fs::FileLock::try_acquire(dir)); ++waited) {
            if (waited == 0)
                mcpp::log::verbose("host-graph", std::format("waiting for {}", path_text(dir)));
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        auto record = read_record(dir / "programs.json");
        std::vector<const Program*> stale;
        for (auto const& p : programs) {
            auto it = record.find(p.member);
            const bool current = it != record.end() && it->is_object()
                && it->value("identity", std::string{}) == p.identity
                && fs::exists(it->value("binary", std::string{}), ec);
            if (current) {
                state.buildProgramBinaries[p.index] = {fs::path(it->value("binary", std::string{})),
                                                       p.identity};
                mcpp::log::verbose("build-program", std::format(
                    "{}: identity {} current", qualified(manifest_of(state, p.index)), p.identity));
                continue;
            }
            mcpp::log::verbose("build-program", std::format(
                "{}: identity {} -> {}, compiled", qualified(manifest_of(state, p.index)),
                it != record.end() && it->is_object() ? it->value("identity", std::string("none"))
                                                      : std::string("none"),
                p.identity));
            stale.push_back(&p);
        }
        if (stale.empty()) {
            // Described, not built: `mcpp emit build-database` lists the graph
            // of programs that are current too.
            if (state.overrides.plan_only) {
                write_group(state, dir, programs, standard, *enginePackage, tc->spec);
                if (auto r = build_group(state, dir, programs, {}, *tc); !r)
                    mcpp::log::verbose("host-graph", r.error());
            }
            continue;
        }
        for (auto const* p : stale) {
            warn_reserved_prefixes(state, p->index);
            if (auto r = check_module_collisions(state, p->index); !r)
                return std::unexpected(r.error());
            if (auto r = check_program_imports(state, p->index, p->source); !r)
                return std::unexpected(r.error());
        }

        write_group(state, dir, programs, standard, *enginePackage, tc->spec);
        const auto began = std::chrono::steady_clock::now();
        for (auto const* p : stale)
            mcpp::build::progress::program_compiling(qualified(manifest_of(state, p->index)),
                                                     p->index == 0 || state.packages[p->index].selectedMember);
        auto built = build_group(state, dir, programs, stale, *tc);
        if (!built) return std::unexpected(built.error());
        const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - began);
        for (auto const* p : stale) {
            const auto bin = built->at(p->member);
            state.buildProgramBinaries[p->index] = {bin, p->identity, took};
            nlohmann::json row;
            row["identity"] = p->identity;
            row["binary"]   = path_text(bin);
            row["package"]  = qualified(manifest_of(state, p->index));
            row["source"]   = p->source.file.empty() ? std::string("synthesised")
                                                     : path_text(p->source.file);
            std::vector<std::string> libs;
            for (auto d : program_libraries(state, p->index))
                libs.push_back(qualified(state.packages[d].manifest));
            row["libraries"] = libs;
            record[p->member] = std::move(row);
        }
        write_if_changed(dir / "programs.json", record.dump(2) + "\n");
    }
    return {};
}

std::expected<BuildProgramBinary, std::string>
build_program_binary(PrepareState& state, std::size_t i) {
    if (auto it = state.buildProgramBinaries.find(i); it != state.buildProgramBinaries.end())
        return it->second;
    if (auto r = ensure_build_programs(state, {i}); !r) return std::unexpected(r.error());
    if (auto it = state.buildProgramBinaries.find(i); it != state.buildProgramBinaries.end())
        return it->second;
    return BuildProgramBinary{};
}

}  // namespace mcpp::build
