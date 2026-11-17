// mcpp.pack.interface — which module units a library package publishes.
//
// THE ASYMMETRY THAT DECIDES THE DESIGN
//
// A prebuilt package ships module interface SOURCE (the consumer has to
// compile it to get a BMI) and a prebuilt library for everything else. So the
// packer has to answer "which .cppm travel?" — and the two ways of getting it
// wrong are not symmetric:
//
//   ship too FEW  → the consumer's compile fails, loudly, naming the module:
//                   `mathkit:secret: error: failed to read compiled module`
//   ship too MANY → a closed-source implementation partition's SOURCE is
//                   published. Nothing fails. Nobody finds out.
//
// One of those is a bug report and the other is a disclosure, which is why the
// set is COMPUTED from the module graph and cannot be hand-written in the
// manifest. An author-supplied list drifts, and it drifts toward the silent
// side as the package evolves.
//
// WHY `.m.o` IS NOT THE ANSWER
//
// The tempting shortcut is "publish the sources of the units that produce a
// BMI". It is wrong, measured: an implementation partition (`module M:impl;`,
// no `export`) also produces a `.m.o`, and its source must NOT be published.
// `.m.o` means "module unit", not "interface".
//
// The same closure has a SECOND use, and getting that one wrong was also
// measured: the archive members to delete before shipping are the objects of
// the units being published as source — not every `.m.o`. Dropping every
// `.m.o` deletes the implementation partition's real code, and every target
// then fails to link with `undefined reference` to a symbol that is nowhere
// in the diagnostic's vicinity.
//
// Design: .agents/docs/2026-08-17-library-distribution-design.md §2.4.2.

export module mcpp.pack.interface;

import std;
import mcpp.modgraph.graph;
import mcpp.source_kind;

export namespace mcpp::pack {

// What `mcpp pack` publishes, withholds, and cannot decide.
struct InterfaceClosure {
    // The module root the closure started from, e.g. "mathkit".
    std::string rootModule;
    // Units whose SOURCE travels, in a stable order (root first, then the
    // order they were reached). Paths are exactly the graph's, so the caller
    // can relativize them against whichever root it knows about.
    std::vector<std::filesystem::path> published;
    // This package's other module units — implementation units, implementation
    // partitions, and any module the interface never reaches. Reported so
    // `mcpp pack` can print both lists: "what you are publishing" is only half
    // of what an author of a closed-source library needs to see.
    std::vector<std::filesystem::path> withheld;
    // Module names the interface imports that NOTHING in this graph provides.
    //
    // The published set is then INCOMPLETE and the consumer's build will fail
    // on it, so this is a hard error at pack time rather than a warning: the
    // whole point of the closure is that the failure lands on the person who
    // can fix it.
    std::vector<std::string> unresolvedImports;
    // Published units that are IMPLEMENTATION partitions (`module M:part;`).
    //
    // Reaching one from the interface's purview is legal and the consumer needs
    // its source to build the BMI at all — so it is published, not refused. But
    // for a closed-source library it is the one outcome nobody wants by
    // accident, and it is invisible in the source: `import :detail;` in an
    // interface looks like any other import. So it is reported, loudly, and
    // `mcpp pack` prints it as a warning naming the file.
    std::vector<std::filesystem::path> publishedImplementationPartitions;
    // Published partitions whose interface-ness NOBODY DETERMINED.
    //
    // A `[scan_overrides."<glob>"]` entry names the modules a file provides and
    // has nowhere to say whether the declaration is exported; a P1689 scanner
    // may omit `is-interface`. Either way mcpp is publishing a partition
    // without knowing which kind it is, and the two outcomes are a working
    // package and a disclosure.
    //
    // Kept apart from the list above because the sentence to print is a
    // different one: that list says "you are publishing your implementation",
    // this one says "mcpp cannot tell whether you are".
    std::vector<std::filesystem::path> publishedUndeterminedPartitions;
};

// Compute the closure for `packageName`, starting at the unit that provides
// `rootModule`.
//
// Only units belonging to `packageName` are followed: an interface may import
// a DEPENDENCY's module, and that module is the dependency's to publish, not
// ours. Such an import is neither published nor unresolved — it is simply not
// this package's business.
std::expected<InterfaceClosure, std::string>
interface_closure(const mcpp::modgraph::Graph& graph,
                  std::string_view packageName,
                  std::string_view rootModule);

// The archive members to delete before shipping: the objects of the units in
// `closure.published`.
//
// Deliberately derived from the closure and not from a file extension — see
// the header for what "delete every .m.o" cost when it was tried.
std::vector<std::string> published_object_names(const InterfaceClosure& closure,
                                                std::string_view objExt = ".o");

} // namespace mcpp::pack

namespace mcpp::pack {

std::expected<InterfaceClosure, std::string>
interface_closure(const mcpp::modgraph::Graph& graph,
                  std::string_view packageName,
                  std::string_view rootModule)
{
    InterfaceClosure out;
    out.rootModule = std::string(rootModule);

    auto owned = [&](const mcpp::modgraph::SourceUnit& u) {
        return u.packageName == packageName;
    };

    // The package's own import of a name, resolved as every import is
    // (mcpp#732): a name two packages provide means the one in its closure.
    auto provider = [&](std::string_view name) {
        return mcpp::modgraph::resolve_provider(graph, packageName, name);
    };
    const auto root = provider(rootModule);
    if (!root) {
        return std::unexpected(std::format(
            "no module interface unit in this build provides '{}'", rootModule));
    }
    if (!owned(graph.units[*root])) {
        return std::unexpected(std::format(
            "module '{}' is provided by package '{}', not '{}'", rootModule,
            graph.units[*root].packageName, packageName));
    }

    std::vector<std::size_t> stack{ *root };
    std::set<std::size_t> seen;
    std::set<std::string> unresolved;

    while (!stack.empty()) {
        auto idx = stack.front();
        stack.erase(stack.begin());          // breadth-first: root first, then its imports
        if (!seen.insert(idx).second) continue;
        auto const& u = graph.units[idx];
        out.published.push_back(u.written());
        // Interface-ness is only ever in question for a PARTITION. `module M;`
        // provides nothing, so the only declaration that can provide a bare
        // `M` is `export module M;` — asking about it there would flag every
        // primary interface of every overridden unit, which is noise, and noise
        // is how a real warning gets ignored.
        if (u.provides && u.provides->logicalName.find(':') != std::string::npos) {
            if (u.providesInterface == false)
                out.publishedImplementationPartitions.push_back(u.written());
            else if (!u.providesInterface.has_value())
                out.publishedUndeterminedPartitions.push_back(u.written());
        }

        for (auto const& req : u.requires_) {
            const auto found = provider(req.logicalName);
            if (!found) {
                // Only OUR module's partitions are our problem. A bare name
                // with no producer is a dependency's module (or `std`), which
                // this package does not publish and must not complain about.
                const bool ours = req.logicalName.starts_with(std::string(rootModule) + ":");
                if (ours) unresolved.insert(req.logicalName);
                continue;
            }
            if (!owned(graph.units[*found])) continue;   // a dependency's unit
            if (!seen.contains(*found)) stack.push_back(*found);
        }
    }

    for (std::size_t i = 0; i < graph.units.size(); ++i) {
        if (!owned(graph.units[i])) continue;
        if (seen.contains(i)) continue;
        out.withheld.push_back(graph.units[i].path);
    }
    out.unresolvedImports.assign(unresolved.begin(), unresolved.end());
    return out;
}

std::vector<std::string> published_object_names(const InterfaceClosure& closure,
                                                std::string_view objExt)
{
    std::vector<std::string> names;
    names.reserve(closure.published.size());
    for (auto const& p : closure.published)
        names.push_back(mcpp::object_filename_for(p, objExt));
    return names;
}

} // namespace mcpp::pack
