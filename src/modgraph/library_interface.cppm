// mcpp.modgraph.library_interface — what a library offers, defined once
// (SPEC-008 v0.2, mcpp#797).
//
// Before this module the answer had two definitions and depended on who asked.
// An ordinary consumer could import every `export module` unit of a
// dependency's sources, and the interface root was read only for warnings; the
// build program's path required the root and took the listed interface units
// beside it. `mcpp pack` and the warning W3 computed the public modules a third
// and a fourth time. Every reader now asks this function:
//
//   the interface root  `[lib] path` (a Form B descriptor's `lib`), else the
//                       convention `src/<name tail>.<interface extension>` when
//                       the file exists; at most one.
//   public modules      the root's module and what it re-exports with
//                       `export import`, transitively; and, for each active
//                       feature, the primary interface units its `sources`
//                       add, with what they re-export. A feature is enabled by
//                       the consumer, and `export import` cannot say "only when
//                       enabled", so a feature's units are that feature's
//                       interface (SPEC-008 I2, as amended).
//   exported modules    every primary module interface the package compiles:
//                       what a consumer can import. One that is exported and
//                       not public draws W3.
//
// A package with no interface root states no interface: its exported modules
// are its public modules (SPEC-008 phase one).

export module mcpp.modgraph.library_interface;

import std;
import mcpp.manifest;
import mcpp.modgraph.graph;
import mcpp.modgraph.scanner;

export namespace mcpp::modgraph {

struct LibraryInterface {
    std::filesystem::path root;            // absolute; empty when the package has none
    bool                  rootDeclared = false;
    std::string           rootModule;
    std::set<std::string> publicModules;   // primary module names
    std::set<std::string> exportedModules; // primary module names
};

// `packageName` is the name the scan recorded on the package's units (its
// qualified name). `activeFeatures` are the features of the instance asked about.
LibraryInterface library_interface(const Graph&                      g,
                                   const mcpp::manifest::Manifest&   manifest,
                                   const std::filesystem::path&      packageRoot,
                                   std::string_view                  packageName,
                                   const std::vector<std::string>&   activeFeatures);

}  // namespace mcpp::modgraph

namespace mcpp::modgraph {

namespace {
std::string primary(std::string_view name) { return std::string(name.substr(0, name.find(':'))); }
}

LibraryInterface library_interface(const Graph& g, const mcpp::manifest::Manifest& manifest,
                                   const std::filesystem::path& packageRoot,
                                   std::string_view packageName,
                                   const std::vector<std::string>& activeFeatures) {
    LibraryInterface out;
    std::map<std::string, std::vector<const SourceUnit*>> unitsOf;
    for (auto const& u : g.units) {
        if (u.packageName != packageName || !u.provides) continue;
        const auto p = primary(u.provides->logicalName);
        unitsOf[p].push_back(&u);
        if (u.provides->logicalName.find(':') == std::string::npos) out.exportedModules.insert(p);
    }
    auto closeOver = [&](std::string start) {
        std::vector<std::string> work{start};
        if (!out.publicModules.insert(start).second) return;
        while (!work.empty()) {
            auto mod = work.back();
            work.pop_back();
            for (auto const* u : unitsOf[mod])
                for (auto const& re : u->reexports)
                    if (auto p = primary(re.logicalName); out.publicModules.insert(p).second)
                        work.push_back(p);
        }
    };

    out.rootDeclared = !manifest.lib.path.empty();
    const auto rootFile = (packageRoot / mcpp::manifest::resolve_lib_root_path(manifest, packageRoot))
                              .lexically_normal();
    std::error_code ec;
    if (std::filesystem::is_regular_file(rootFile, ec)) {
        out.root = rootFile;
        for (auto const& u : g.units)
            if (u.packageName == packageName && u.provides
                && u.path.lexically_normal() == rootFile) {
                out.rootModule = primary(u.provides->logicalName);
                break;
            }
    }

    if (out.rootModule.empty()) {
        out.publicModules = out.exportedModules;
    } else {
        closeOver(out.rootModule);
    }
    for (auto const& f : activeFeatures) {
        auto it = manifest.buildConfig.featureSources.find(f);
        if (it == manifest.buildConfig.featureSources.end()) continue;
        std::set<std::filesystem::path> files;
        for (auto const& glob : it->second)
            for (auto const& file : expand_glob(packageRoot, glob))
                files.insert(file.lexically_normal());
        for (auto const& u : g.units)
            if (u.packageName == packageName && u.provides
                && u.provides->logicalName.find(':') == std::string::npos
                && files.contains(u.path.lexically_normal()))
                closeOver(primary(u.provides->logicalName));
    }
    return out;
}

}  // namespace mcpp::modgraph
