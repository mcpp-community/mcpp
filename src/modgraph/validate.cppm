// mcpp.modgraph.validate — naming/lint + topology checks.

export module mcpp.modgraph.validate;

import std;
import mcpp.manifest;
import mcpp.modgraph.graph;
import mcpp.modgraph.scanner;

export namespace mcpp::modgraph {

struct ValidateError {
    std::filesystem::path           path;        // optional
    std::string                     message;
};

struct ValidateReport {
    std::vector<ValidateError>      errors;
    std::vector<ValidateError>      warnings;
    std::vector<std::size_t>        topoOrder;  // valid only if errors empty
    bool ok() const { return errors.empty(); }
};

ValidateReport validate(const Graph&                    g,
                        const mcpp::manifest::Manifest& manifest);

// Same as `validate` plus a project-root path used to verify that the
// lib-root convention file actually exists on disk. Pass an empty path
// to disable the on-disk check (used by unit tests that build a Graph
// in memory without writing source files).
ValidateReport validate(const Graph&                    g,
                        const mcpp::manifest::Manifest& manifest,
                        const std::filesystem::path&    projectRoot);

bool is_public_package_name(std::string_view name);
bool is_forbidden_top_module(std::string_view name);

} // namespace mcpp::modgraph

namespace mcpp::modgraph {

bool is_public_package_name(std::string_view name) {
    return name.find('.') != std::string_view::npos;
}

bool is_forbidden_top_module(std::string_view name) {
    static constexpr std::string_view blacklist[] = {
        "core", "util", "common", "std", "detail", "internal", "base"
    };
    // top = before first '.'
    auto p = name.find('.');
    auto top = (p == std::string_view::npos) ? name : name.substr(0, p);
    for (auto b : blacklist) if (top == b) return true;
    return false;
}

ValidateReport validate(const Graph&                    g,
                        const mcpp::manifest::Manifest& manifest)
{
    return validate(g, manifest, /*projectRoot=*/{});
}

ValidateReport validate(const Graph&                    g,
                        const mcpp::manifest::Manifest& manifest,
                        const std::filesystem::path&    projectRoot)
{
    ValidateReport r;

    // 1. Naming: public packages' exported modules must be prefixed by package name,
    //    and the top-level segment must not be in the forbidden list (for public pkgs).
    //
    // Each SourceUnit carries its OWN package name (set by the scanner) so this
    // works transparently for multi-package builds (path deps): each unit is
    // validated against its own manifest's package name, not the primary's.
    // 0.0.10+: module naming is the library author's choice. The build tool
    // no longer enforces that module names must be prefixed by the package
    // name. Only the forbidden-top-level-name check is retained to avoid
    // collisions with well-known module names (std, core, etc.).
    for (auto& u : g.units) {
        if (!u.provides) continue;
        const auto& m = u.provides->logicalName;
        auto base = m.substr(0, m.find(':'));      // strip partition suffix

        if (is_forbidden_top_module(base)) {
            r.errors.push_back({u.path,
                std::format("module '{}' uses a forbidden top-level name (core/util/common/...)", m)});
        }
    }

    // 2. modules.exports vs scanner result.
    // Only checks the primary manifest's package — dep packages have their
    // own [modules].exports validated by their own builds.
    if (!manifest.modules.exports_.empty()) {
        std::set<std::string> declared(manifest.modules.exports_.begin(),
                                       manifest.modules.exports_.end());
        std::set<std::string> actual;
        for (auto& u : g.units) {
            if (u.provides && u.packageName == manifest.package.name) {
                auto base = u.provides->logicalName.substr(0, u.provides->logicalName.find(':'));
                actual.insert(base);
            }
        }
        // Any actual module that isn't in declared is a violation.
        for (auto& m : actual) {
            if (!declared.contains(m)) {
                r.errors.push_back({{},
                    std::format("module '{}' is exported by code but not listed in [modules].exports", m)});
            }
        }
        if (manifest.modules.strict) {
            for (auto& m : declared) {
                if (!actual.contains(m)) {
                    r.errors.push_back({{},
                        std::format("module '{}' declared in [modules].exports but never exported (strict)", m)});
                }
            }
        }
    }

    // 2.5 Lib-root convention (M5.x+).
    //
    // For projects that ship a `kind = "lib"` target, check that the
    // lib-root file exists. The lib root is either `[lib].path` (explicit
    // override) or `src/<package-tail>.cppm` (default convention).
    //
    // 0.0.10+: the module name exported by the lib root is NOT required
    // to match [package].namespace + name. The library author decides
    // the module name; the build tool auto-detects it via the scanner.
    // Only structural correctness is enforced (file exists, no partition).
    //
    // Pure-binary projects (mcpp itself, scaffolded `mcpp new`) skip this
    // check — they have no lib-root concept.
    if (mcpp::manifest::has_lib_target(manifest)) {
        auto lib_root_rel = mcpp::manifest::resolve_lib_root_path(manifest);
        const bool was_explicit = !manifest.lib.path.empty();

        // On-disk existence check (skipped when projectRoot is empty —
        // unit tests can build Graphs in memory without writing files).
        if (!projectRoot.empty()) {
            auto lib_root_abs = lib_root_rel.is_absolute()
                ? lib_root_rel
                : (projectRoot / lib_root_rel);
            std::error_code ec;
            const bool exists = std::filesystem::exists(lib_root_abs, ec);
            if (!exists) {
                if (was_explicit) {
                    r.errors.push_back({lib_root_rel, std::format(
                        "[lib].path '{}' does not exist", lib_root_rel.string())});
                } else {
                    r.warnings.push_back({lib_root_rel, std::format(
                        "lib target without conventional lib root '{}' "
                        "(create the file or set [lib].path)",
                        lib_root_rel.string())});
                }
            }
        }

        // If the lib-root file is in the graph, verify it exports a
        // primary module (not a partition). The actual module name is
        // the library author's choice — no name-matching enforced.
        const mcpp::modgraph::SourceUnit* lib_unit = nullptr;
        for (auto& u : g.units) {
            auto u_rel = u.path.is_absolute() && !projectRoot.empty()
                ? std::filesystem::relative(u.path, projectRoot)
                : u.path;
            if (u_rel == lib_root_rel || u.path == lib_root_rel) {
                lib_unit = &u;
                break;
            }
        }
        if (lib_unit) {
            if (!lib_unit->provides) {
                r.errors.push_back({lib_unit->path, std::format(
                    "lib root '{}' must declare `export module <name>;`",
                    lib_root_rel.string())});
            } else {
                const auto& m = lib_unit->provides->logicalName;
                if (m.find(':') != std::string::npos) {
                    r.errors.push_back({lib_unit->path, std::format(
                        "lib root '{}' exports a partition '{}' — "
                        "must be a primary module (no `:partition` suffix)",
                        lib_root_rel.string(), m)});
                }
            }
        }
    }

    // 3. Topology
    auto topo = topo_sort(g);
    if (!topo) {
        std::string names;
        for (auto i : topo.error().cycle) {
            if (i < g.units.size()) {
                names += g.units[i].path.string() + " ";
            }
        }
        r.errors.push_back({{},
            std::format("circular module dependency among: {}", names)});
    } else {
        r.topoOrder = std::move(*topo);
    }

    return r;
}

} // namespace mcpp::modgraph
