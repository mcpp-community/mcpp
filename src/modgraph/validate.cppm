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
    ValidateReport r;

    // 1. Naming: public packages' exported modules must be prefixed by package name,
    //    and the top-level segment must not be in the forbidden list (for public pkgs).
    //
    // Each SourceUnit carries its OWN package name (set by the scanner) so this
    // works transparently for multi-package builds (path deps): each unit is
    // validated against its own manifest's package name, not the primary's.
    for (auto& u : g.units) {
        if (!u.provides) continue;
        const auto& m = u.provides->logicalName;
        auto base = m.substr(0, m.find(':'));      // strip partition suffix
        const auto& pkg_name = u.packageName;       // ← unit's own package
        const bool is_public = is_public_package_name(pkg_name);

        if (is_public) {
            if (base != pkg_name && !base.starts_with(pkg_name + ".")) {
                r.errors.push_back({u.path,
                    std::format("public module '{}' must be prefixed by package name '{}'",
                                m, pkg_name)});
            }
            if (is_forbidden_top_module(base)) {
                r.errors.push_back({u.path,
                    std::format("module '{}' uses a forbidden top-level name (core/util/common/...)", m)});
            }
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
