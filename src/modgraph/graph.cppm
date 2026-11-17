// mcpp.modgraph.graph — DAG of module units (per-package, cross-package later).

export module mcpp.modgraph.graph;

import std;
import mcpp.source_kind;
import mcpp.graph;

export namespace mcpp::modgraph {

struct ModuleId {
    std::string                     logicalName;       // "mcpplibs.hello.core"

    auto operator<=>(const ModuleId&) const = default;
};

// THE MODULE DECLARATION A UNIT CARRIES, AS THE SCANNER READ IT.
//
// `provides`, `providesInterface` and `requires_` describe the module GRAPH, and
// one declaration form is not recoverable from them: `module M;` (an
// implementation unit of M) is recorded as `requires M`, exactly as a unit that
// only says `import M;`. The form is what a build database states as a unit's
// role, and a reader has to be told it rather than guess it (S1 profile 0.2.0
// §8.2: a producer that cannot decide writes `unknown`).
//
//   None                     no module declaration
//   Interface                `export module M;`
//   InterfacePartition       `export module M:P;`
//   ImplementationPartition  `module M:P;`
//   Implementation           `module M;`
//   Unknown                  the declaration was not read: a `scan_overrides`
//                            entry, or a P1689 record that cannot tell an
//                            implementation unit from an importer
enum class ModuleDeclaration {
    None,
    Interface,
    InterfacePartition,
    ImplementationPartition,
    Implementation,
    Unknown,
};

struct SourceUnit {
    std::filesystem::path           path;
    // mcpp#233: path relative to this unit's PACKAGE ROOT (not the primary
    // project root — a path dependency has its own root), set by the
    // scanner where the root is known. Used by plan.cppm to mirror the
    // source's directory layout into its object path so that two
    // same-named files under different subdirectories (e.g. a/src/util.cpp
    // and b/src/util.cpp) never fold onto the same object output. Left
    // default-constructed (empty) for units synthesized outside the
    // scanner (e.g. plan.cppm's ad-hoc main.cpp CompileUnit, which never
    // reads this field).
    std::filesystem::path           relPath;
    std::string                     packageName;
    std::vector<std::filesystem::path> localIncludeDirs;
    // #249: dirs emitted as -idirafter — searched after the toolchain's
    // system dirs, so a dep's source root can't shadow standard headers.
    std::vector<std::filesystem::path> localIncludeDirsAfter;
    std::vector<std::string>        packageCflags;
    std::vector<std::string>        packageCxxflags;
    std::vector<std::string>        packageAsmflags;   // per-glob asmflags (G4)
    std::optional<ModuleId>         provides;
    // Was `provides` declared with `export module`?
    //
    // Both spellings produce a BMI and an object, so `provides` alone cannot
    // tell an INTERFACE partition (`export module M:api;`) from an
    // IMPLEMENTATION partition (`module M:impl;`) — and the difference decides
    // whether a source may be published. `mcpp pack` publishes the module
    // closure of the lib root; a closure that reaches an implementation
    // partition has to publish it too (the consumer cannot build the BMI
    // without it), and the author needs to be told that their implementation
    // is going out.
    //
    // Three states, because "nobody determined this" is a real answer and it
    // used to be spelled `true`:
    //
    //   true     `export module M:api;` — the text scanner read the keyword
    //   false    `module M:impl;`       — likewise
    //   nullopt  nobody could tell: a `scan_overrides` entry names the module
    //            but has no way to say whether it is exported, and a P1689
    //            scanner may omit `is-interface`
    //
    // It defaulted to `true` and was called the conservative direction "since
    // the flag only ever produces a warning". That had it backwards: `true` is
    // the value that produces NO warning, so an undetermined implementation
    // partition was published in silence — the exact failure this field exists
    // to prevent. Unknown now warns, naming the file and the reason.
    std::optional<bool>             providesInterface;
    std::vector<ModuleId>           requires_;
    // The subset of `requires_` imported with `export import` (the text
    // scanner; a P1689 scan does not report it and leaves this empty).
    std::vector<ModuleId>           reexports;
    // The declaration form (see ModuleDeclaration). Read by the build database
    // renderer (mcpp.build.build_database), which states it as the unit's role.
    ModuleDeclaration               declaration = ModuleDeclaration::None;
    // The unit's ROLE, decided once by the scanner from the owning package's
    // extension table and carried from here on. Every downstream consumer
    // (planner, backend, compile_commands, the asm dialect check) reads this
    // instead of re-deriving it from the extension — see mcpp.source_kind for
    // why that mattered.
    mcpp::SourceKind                kind = mcpp::SourceKind::Other;
    //
    // `isModuleInterface` / `isImplementation` used to live here. They were
    // WRITE-ONLY: three sites derived them (the scanner said `.cpp`, the p1689
    // reader said `.cpp || .cxx`, the scan_overrides branch said "has
    // provides"), the three disagreed, and nothing in src/ or tests/ ever read
    // the result. Converging three derivations of a value nobody reads is
    // still three derivations, so they are gone instead. `provides` answers
    // "is this an interface"; `kind` answers the rest.
    // Unit built from a manifest scan_overrides declaration instead of a
    // real scan — plan-vs-ddi verification is mandatory for these.
    bool                            scanOverridden     = false;
    // The source file this unit is built from, when the unit is not that file:
    // a unit the source seam gave for it (a private module fragment as a unit
    // of its own, SPEC-019). Empty: `path` is the source as written. What
    // answers "which units is this file built as" reads it.
    std::filesystem::path           writtenAs;
};

// A package's closure: the qualified names of the packages whose modules its
// units may import, itself included (mcpp#732). It is the package and every
// package it reaches through code and workspace-member edges; an `artifacts`
// edge ships a separate program and is not followed.
using Closures = std::map<std::string, std::set<std::string>, std::less<>>;

struct Graph {
    std::vector<SourceUnit>                                  units;
    // logical-name -> index into units, for a name ONE unit of the graph
    // provides. A name two packages provide (mcpp#732) is in providersOf only.
    std::map<std::string, std::size_t, std::less<>>             producerOf;
    // logical-name -> every unit that provides it, in unit order.
    std::map<std::string, std::vector<std::size_t>, std::less<>> providersOf;
    // The closure of each package the plan compiles. Empty when the caller has
    // none to give, and then a name has to be unique in the whole graph.
    Closures                                                 closures;
    // edges as (consumer-index, producer-index)
    std::vector<std::pair<std::size_t, std::size_t>>         edges;
};

// WHICH PROVIDER AN IMPORT MEANS (mcpp#732). A module name identifies one
// module within one program: GCC and clang mangle a module's entities with its
// name and give it one initializer named after it, so two modules of one name
// cannot be linked into one program, and two programs may each have one. A
// build configuration holds several programs (a package and the programs it
// ships through `artifacts`, a workspace's members), so a name is resolved in
// the importer's closure, not in the whole configuration.
//
// Among `providerPackages`, the packages that provide one name, the index of
// the one `importer` means: the only provider, or else the only one in the
// importer's closure. Nothing when the closure holds none of them or more than
// one. The one rule the scanner, the plan, the backend and the packer use.
std::optional<std::size_t> choose_provider(std::span<const std::string> providerPackages,
                                           std::string_view importer,
                                           const Closures& closures);
// The unit of `g` that `importer`'s import of `name` means.
std::optional<std::size_t> resolve_provider(const Graph& g, std::string_view importer,
                                            std::string_view name);

// Topological order: returns indices of units in producer-before-consumer order.
// Returns std::unexpected with the cycle if any, as the ordered path that
// walks it (see mcpp.graph) rather than merely the units left over.
struct CycleError {
    std::vector<std::size_t> cycle;
};
std::expected<std::vector<std::size_t>, CycleError> topo_sort(const Graph& g);

} // namespace mcpp::modgraph

namespace mcpp::modgraph {

std::optional<std::size_t> choose_provider(std::span<const std::string> providerPackages,
                                           std::string_view importer,
                                           const Closures& closures) {
    if (providerPackages.size() == 1) return 0;
    auto closure = closures.find(importer);
    if (closure == closures.end()) return std::nullopt;
    std::optional<std::size_t> found;
    for (std::size_t i = 0; i < providerPackages.size(); ++i) {
        if (!closure->second.contains(providerPackages[i])) continue;
        if (found) return std::nullopt;
        found = i;
    }
    return found;
}

std::optional<std::size_t> resolve_provider(const Graph& g, std::string_view importer,
                                            std::string_view name) {
    auto it = g.providersOf.find(name);
    if (it == g.providersOf.end() || it->second.empty()) {
        // A graph built without `providersOf` (by hand, in a test) states its
        // single providers in `producerOf`.
        if (auto p = g.producerOf.find(name); p != g.producerOf.end()) return p->second;
        return std::nullopt;
    }
    std::vector<std::string> packages;
    packages.reserve(it->second.size());
    for (auto i : it->second) packages.push_back(g.units[i].packageName);
    auto chosen = choose_provider(packages, importer, g.closures);
    if (!chosen) return std::nullopt;
    return it->second[*chosen];
}

std::expected<std::vector<std::size_t>, CycleError> topo_sort(const Graph& g) {
    // g.edges: (consumer, producer) pairs, "consumer depends on producer".
    // The unit order is the order of the objects on a link line, which
    // Mach-O uses as the initializer order, so this is the order the module
    // graph has always had: Kahn's algorithm with the ready units on a stack,
    // over the edges in the order the graph records them (see mcpp.graph).
    auto order = mcpp::graph::stack_topological_order(g.units.size(), g.edges);
    if (!order) return std::unexpected(CycleError{std::move(order.error().cycle)});
    return std::move(*order);
}

} // namespace mcpp::modgraph
